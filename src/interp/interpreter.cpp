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
#include "debug_flags.h"
#include "frontend/dynamic_linker.h"
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
    // ── TEMP: NSS trace ─────────────────────────────────────────
    {
        static bool nss_trace_ = dbg().nss;
        if (nss_trace_) {
            static bool inited_ = false;
            static uint64_t base_ = 0;
            if (!inited_) {
                // resolved at first call; libc maps at 0x500191e000
                inited_ = true;
            }
            const uint64_t pc = cpu.pc;
            if ((pc == 0x5001a220a0ULL) || (pc == 0x5001a22180ULL) ||
                (pc == 0x5001a21d40ULL) || (pc == 0x5001a22620ULL) ||
                (pc == 0x5001a31608ULL) || (pc == 0x5001a30f80ULL) ||
                (pc == 0x5001a22ce0ULL) || (pc == 0x5001a32750ULL) ||
                (pc == 0x5001a368a0ULL) || (pc == 0x5001a369a0ULL)) {
                fprintf(stderr, "[nsstrace] pc=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x19=0x%llx x23=0x%llx sp=0x%llx\n",
                        (unsigned long long)pc,
                        (unsigned long long)cpu.regs[0], (unsigned long long)cpu.regs[1],
                        (unsigned long long)cpu.regs[2], (unsigned long long)cpu.regs[19],
                        (unsigned long long)cpu.regs[23], (unsigned long long)cpu.sp);
                fflush(stderr);
            }
        }
    }
    // ── TEMP: xcb_create_window arg trace ────────────────────────
    {
        static bool xcb_cw_en = dbg().xcb_cw;
        if (xcb_cw_en) {
            static uint64_t xcb_cw_addr_ = 0;
            static bool xcb_cw_resolved_ = false;
            if (!xcb_cw_resolved_ && dyn_linker_) {
                xcb_cw_addr_ = dyn_linker_->resolve_symbol("xcb_create_window");
                if (xcb_cw_addr_) { xcb_cw_resolved_ = true; }
            }
            if (xcb_cw_addr_ && cpu.pc == xcb_cw_addr_) {
                fprintf(stderr, "[XCB_CW] pc=0x%llx depth=%u wid=0x%llx par=0x%llx x=%d y=%d w=%u h=%u border=%u class=%u vis=0x%llx mask=0x%llx vlist=0x%llx\n",
                        (unsigned long long)cpu.pc,
                        (unsigned)(cpu.regs[1] & 0xff), (unsigned long long)cpu.regs[2],
                        (unsigned long long)cpu.regs[3], (int)cpu.regs[4], (int)cpu.regs[5],
                        (unsigned)cpu.regs[6], (unsigned)cpu.regs[7],
                        (unsigned)cpu.regs[8], (unsigned)cpu.regs[9],
                        (unsigned long long)cpu.regs[10], (unsigned long long)cpu.regs[11]);
                fflush(stderr);
            }
        }
    }
    // ── TEMP: xcb image-blit entry trace ────────────────────────────
    {
        static bool xcb_img_en = dbg().xcb_img;
        if (xcb_img_en) {
            static const char* kNames[] = {
                "xcb_put_image", "xcb_wait_for_event", "xcb_poll_for_event",
                "xcb_poll_for_queued_event", "xcb_flush",
                "xcb_wait_for_reply", "xcb_poll_for_reply",
                "xcb_writev", "xcb_connection_has_error", "xcb_connect",
            };
            // NO_ASLR: libxcb base = 0x500562c000.
            static const uint64_t kAddrs[10] = {
                0x500562c000 + 0x19150ULL,  // xcb_put_image
                0x500562c000 + 0xe650ULL,   // xcb_wait_for_event
                0x500562c000 + 0xf5d0ULL,   // xcb_poll_for_event
                0x500562c000 + 0xf5e4ULL,   // xcb_poll_for_queued_event
                0x500562c000 + 0xd8b4ULL,   // xcb_flush
                0x500562c000 + 0xe420ULL,   // xcb_wait_for_reply
                0x500562c000 + 0xf2e4ULL,   // xcb_poll_for_reply
                0x500562c000 + 0xc920ULL,   // xcb_writev
                0x500562c000 + 0xbe40ULL,   // xcb_connection_has_error
                0x500562c000 + 0x10730ULL,  // xcb_connect
            };
            for (int i = 0; i < 10; i++) {
                if (kAddrs[i] && cpu.pc == kAddrs[i]) {
                    fprintf(stderr, "[XCBIMG] t%d %s pc=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx x4=0x%llx x5=0x%llx x6=0x%llx x7=0x%llx\n",
                            cpu.tid, kNames[i], (unsigned long long)cpu.pc,
                            (unsigned long long)cpu.regs[0], (unsigned long long)cpu.regs[1],
                            (unsigned long long)cpu.regs[2], (unsigned long long)cpu.regs[3],
                            (unsigned long long)cpu.regs[4], (unsigned long long)cpu.regs[5],
                            (unsigned long long)cpu.regs[6], (unsigned long long)cpu.regs[7]);
                    fflush(stderr);
                }
            }
        }
    }
    // ── Decode via the shared decoder ─────────────────────────────
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
                    // NOTE: `LSR Xd, Xn, #0` (UBFM #0, #(datasize-1)) is a
                    // NO-OP on real AArch64 — a shift by zero returns the
                    // source unchanged. A previous "fix" claimed hardware
                    // returns 0 here and zeroed the register, which broke
                    // W-form `lsr w3, x19, #0` used by compilers to grab
                    // the low 32 bits of a 64-bit constant (it yielded 0,
                    // corrupting vec3/vec4 struct packing). The general
                    // extract path below (ROR by immr == 0, full-width
                    // mask) already yields the correct source value for
                    // UBFM, SBFM, and BFM, so no special case is needed.
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
                    // nzcv_field is a 4-bit value: N=bit3, Z=bit2, C=bit1, V=bit0.
                    // 8 (non-zero = true) instead of 1. This corrupted the
                    // flags, causing strlen's CCMP to produce wrong results.
                    cpu.set_flag_n((d.nzcv_field >> 3) & 1);
                    cpu.set_flag_z((d.nzcv_field >> 2) & 1);
                    cpu.set_flag_c((d.nzcv_field >> 1) & 1);
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
                        if (b != 0) {
                            // ARM: SDIV of INT_MIN / -1 = INT_MIN (no #DE).
                            // Bare signed division is x86 idiv #DE → SIGFPE.
                            if (width == 64) {
                                int64_t sa = static_cast<int64_t>(a), sb = static_cast<int64_t>(b);
                                res = (sb == -1 && sa == INT64_MIN) ? static_cast<uint64_t>(INT64_MIN)
                                    : static_cast<uint64_t>(sa / sb);
                            } else {
                                int32_t sa = static_cast<int32_t>(a), sb = static_cast<int32_t>(b);
                                res = (sb == -1 && sa == INT32_MIN) ? static_cast<uint64_t>(static_cast<uint32_t>(INT32_MIN))
                                    : static_cast<uint64_t>(static_cast<uint32_t>(sa / sb));
                            }
                        }
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
                        // around — e.g. ROR(0x12345678, 7) returned 0x02468acf
                        // instead of 0xf2468acf. This broke MD5 (which uses
                        // 32-bit rotates in every round) and any other code
                        // using ROR — the JIT was correct, the interpreter
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
            case InstClass::CRC32: {
                // CRC32/CRC32C instructions.
                // d.imm: 0=byte, 1=halfword, 2=word, 3=doubleword
                // d.is_sub: 0=CRC32 (poly 0x04C11DB7), 1=CRC32C (Castagnoli poly 0x1EDC6F41)
                // Operands: Rn=accumulated CRC, Rm=data to process
                // Result: new CRC value (32-bit, always zero-extended to 64-bit)
                uint32_t crc = static_cast<uint32_t>(cpu.regs[d.rn]);
                uint64_t data = cpu.regs[d.rm];
                // CRC32 polynomial tables (computed inline)
                static const uint32_t crc32_poly[256] = {
                    0x00000000,0x77073096,0xEE0E612C,0x990951BA,0x076DC419,0x706AF48F,0xE963A535,0x9E649514,
                    0x0EDB8832,0x79DCB8A4,0xE0D5E91E,0x97D2D988,0x09B64C2B,0x7EB17CBE,0xE7B82D71,0x90BF1D2F,
                    0x1DB71064,0x6AB020F2,0xF3B97148,0x84BE41DE,0x1ADAD47D,0x6DDDE4EB,0xF4D4B551,0x83D385C7,
                    0x136C9856,0x646BA8C0,0xFD62F97A,0x8A65C9EC,0x14015C4F,0x63066CD9,0xFA0F3D63,0x8D080DF5,
                    0x3B6E20C8,0x4C69105E,0xD56041E4,0xA2677172,0x3C03E4D1,0x4B04D447,0xD20D85FD,0xA50AB56B,
                    0x35B5A8FA,0x42B2986C,0xDBBBC9D6,0xACBCF940,0x32D86CE3,0x45DF5C75,0xDCD60DCF,0xABD13D59,
                    0x26D930AC,0x51DE003A,0xC8D75180,0xBFD06116,0x21B4F4B5,0x56B3C423,0xCFBA9599,0xB8BDA50F,
                    0x2802B89E,0x5F058808,0xC60CD9B2,0xB10BE924,0x2F6F7C87,0x58684C11,0xC1611DAB,0xB6662D3D,
                    0x76DC4190,0x01DB7106,0x98D220BC,0xEFD5102A,0x71B18589,0x06B6B51F,0x9FBFE4A5,0xE8B8D433,
                    0x7807C9A2,0x0F00F934,0x9609A88E,0xE10E9818,0x7F6A0DBB,0x086D3D2D,0x91646C97,0xE6635C01,
                    0x6B6B51F4,0x1C6C6162,0x856530D8,0xF262004E,0x6C0695ED,0x1B01A57B,0x8208F4C1,0xF50FC457,
                    0x65B0D9C6,0x12B7E950,0x8BBEB8EA,0xFCB9887C,0x62DD1DDF,0x15DA2D49,0x8CD37CF3,0xFBD44C65,
                    0x4DB26158,0x3AB551CE,0xA3BC0074,0xD4BB30E2,0x4ADFA541,0x3DD895D7,0xA4D1C46D,0xD3D6F4FB,
                    0x4369E96A,0x346ED9FC,0xAD678846,0xDA60B8D0,0x44042D73,0x33031DE5,0xAA0A4C5F,0xDD0D7CC9,
                    0x5005713C,0x270241AA,0xBE0B1010,0xC90C2086,0x5768B525,0x206F85B3,0xB966D409,0xCE61E49F,
                    0x5EDEF90E,0x29D9C998,0xB0D09822,0xC7D7A8B4,0x59B33D17,0x2EB40D81,0xB7BD5C3B,0xC0BA6CAD,
                    0xEDB88320,0x9ABFB3B6,0x03B6E20C,0x74B1D29A,0xEAD54739,0x9DD277AF,0x04DB2615,0x73DC1683,
                    0xE3630B12,0x94643B84,0x0D6D6A3E,0x7A6A5AA8,0xE40ECF0B,0x9309FF9D,0x0A00AE27,0x7D079EB1,
                    0xF00F9344,0x8708A3D2,0x1E01F268,0x6906C2FE,0xF762575D,0x806567CB,0x196C3671,0x6E6B06E7,
                    0xFED41B76,0x89D32BE0,0x10DA7A5A,0x67DD4ACC,0xF9B9DF6F,0x8EBEEFF9,0x17B7BE43,0x60B08ED5,
                    0xD6D6A3E8,0xA1D1937E,0x38D8C2C4,0x4FDFF252,0xD1BB67F1,0xA6BC5767,0x3FB506DD,0x48B2364B,
                    0xD80D2BDA,0xAF0A1B4C,0x36034AF6,0x41047A60,0xDF60EFC3,0xA867DF55,0x316E8EEF,0x4669BE79,
                    0xCB61B38C,0xBC66831A,0x256FD2A0,0x5268E236,0xCC0C7795,0xBB0B4703,0x220216B9,0x5505262F,
                    0xC5BA3BBE,0xB2BD0B28,0x2BB45A92,0x5CB36A04,0xC2D7FFA7,0xB5D0CF31,0x2CD99E8B,0x5BDEAE1D,
                    0x9B64C2B0,0xEC63F226,0x756AA39C,0x026D930A,0x9C0906A9,0xEB0E363F,0x72076785,0x05005713,
                    0x95BF4A82,0xE2B87A14,0x7BB12BAE,0x0CB61B38,0x92D28E9B,0xE5D5BE0D,0x7CDCEFB7,0x0BDBDF21,
                    0x86D3D2D4,0xF1D4E242,0x68DDB3F8,0x1FDA836E,0x81BE16CD,0xF6B9265B,0x6FB077E1,0x18B74777,
                    0x88085AE6,0xFF0F6A70,0x66063BCA,0x11010B5C,0x8F659EFF,0xF862AE69,0x616BFFD3,0x166CCF45,
                    0xA00AE278,0xD70DD2EE,0x4E048354,0x3903B3C2,0xA7672661,0xD06016F7,0x4969474D,0x3E6E77DB,
                    0xAED16A4A,0xD9D65ADC,0x40DF0B66,0x37D83BF0,0xA9BCAE53,0xDEBB9EC5,0x47B2CF7F,0x30B5FFE9,
                    0xBDBDF21C,0xCABAC28A,0x53B39333,0x24B4A3A6,0xBAD03605,0xCDD70693,0x54DE5729,0x23D967BF,
                    0xB3667A2E,0xC4614AB8,0x5D681B02,0x2A6F2B94,0xB40BBE37,0xC30C8EA1,0x5A05DF1B,0x2D02EF8D,
                };
                static const uint32_t crc32c_poly[256] = {
                    0x00000000,0xF26B8303,0xE13B70F7,0x1350F3F4,0xC79A971F,0x35F1141C,0x26A1E7E8,0xD4CA64EB,
                    0x8AD958CF,0x78B2DBCC,0x6BE22838,0x9989AB3B,0x4D43CFD0,0xBF284CD3,0xAC78BF27,0x5E133C24,
                    0x105EC76F,0xE235446C,0xF165B798,0x030E349B,0xD7C45070,0x251D3B73,0xF1C87A86,0x03183285,
                    0x9AAB15D0,0x68C0AAC3,0x5A6366A9,0xA86455AA,0xCCCCCB21,0x3EA55D22,0x64935876,0x96A69E75,
                    0xE1EC6AB0,0x1387E4B3,0x63D7F747,0x91B4C944,0xC5A4CF8F,0x373AB88C,0x3A4D9078,0xC816D07B,
                    0x6B8FE6D5,0x99E03E66,0x92AA0368,0x60B8D436,0xCB5A37CC,0x391C0E6F,0x32B43C81,0xC0E0F2C2,
                    0x4B8B0D93,0xB98D2D90,0x6E2C1BE4,0x9C47FBE7,0x7C881699,0x8E73B07A,0xD57A1F65,0x27E10D06,
                    0xDAA1103D,0x28B41D3E,0xC522B6FF,0x3719C6FC,0xC8B5E2C9,0x3A5D8CAA,0x9C26939F,0x6E4E8FA8,
                    0x524F1D60,0xA06C1C23,0x88F22C96,0x7AC46B35,0x55F03A9C,0xA745C136,0x7611C84E,0x842C0A0D,
                    0x9CFECE0A,0x6E945959,0x6D7FA2C8,0x9F56F9C2,0x9E36D0BC,0x6C59D283,0x667043C2,0x94C56573,
                    0x930441E5,0x1C7B6E66,0x9C6B5BF2,0x6E819C73,0x9ED59E4E,0x6C5A744D,0x6158E5A6,0x9381C6F7,
                    0x77C7E2F0,0x459CC2C3,0x7D3F845D,0x8F4C095E,0x55A83F04,0xA741F317,0x95B0C4C5,0x67BD6CE6,
                    0x5C539C18,0xAE848B6B,0x9C668B2F,0x6E06B5BC,0x9CF5A473,0x6D9E2E40,0x6D6D6168,0x9F4A7FE3,
                    0x6E4DC0B4,0x9CF7B9C7,0x6D5BC918,0x9D6F45CA,0x6E0B86E9,0x9CF32C6A,0x6C5B149F,0x9F2EC6A4,
                    0x6E7E19D3,0x9CF2C32E,0x6D741D7F,0x9D5F2591,0x6C8D7C7A,0x9C0B5FF6,0x67381BA5,0x95C70F94,
                    0x6E51B8E3,0x9CC4A7E0,0x6CC89ACF,0x9E19FA2C,0x6CE5BE29,0x9CFAB90B,0x6D6B36F4,0x9F46E835,
                    0x6CC7C6C0,0x9E16BE23,0x6D7AC1F7,0x9F556164,0x6CFB938C,0x9E1F6ECB,0x6D7DC09E,0x9F41A4FF,
                    0x6CD7C2C8,0x9F0FCA43,0x6D29A8B0,0x9F59CE91,0x6C2E5722,0x9C6BCAB9,0x653DF2D0,0x9D45B613,
                    0x6C7DDEAA,0x9E4E9DC9,0x6D1DEBF6,0x9C7B2FA5,0x6D3D7C9C,0x9F76F08B,0x6C8B6A3A,0x9C3C2C89,
                    0x6D69F0E1,0x9C5C6F36,0x6C5F9DA7,0x9F4E4A82,0x6CA1E6BC,0x9E1D24BB,0x6CB59E50,0x9F1FA613,
                    0x6CA5D2A6,0x9E7CF8F5,0x6D2E8364,0x9F12E6A7,0x6C9F2E1E,0x9F668F79,0x6D2C4268,0x9F60D0B6,
                    0x6C7A1D1B,0x9E0E7138,0x6D5CBAC9,0x9F0B4D98,0x6CB4A3D7,0x9F48E50C,0x6D44B89A,0x9C5C8363,
                    0x6CC3B2C6,0x9F5F2BC1,0x6D22CD6E,0x9F3687E4,0x6CA3A7B8,0x9E6E04F7,0x6C1B6DE2,0x9F3FA366,
                    0x6D32C5C1,0x9E089BA2,0x6C5D782B,0x9F58FA3B,0x6CFB72E4,0x9C3A13C6,0x6CB31868,0x9F7F2FFB,
                    0x6D2D55B5,0x9F62A10C,0x6C9CC565,0x9D2F0B9E,0x6C7CFEC2,0x9E7C5C2B,0x6D6A6BB6,0x9D2D368D,
                    0x6D4B7498,0x9F2C7E2E,0x6C5BA0C1,0x9F736AF8,0x6CD08FF7,0x9D31DB06,0x6C66B7F1,0x9F1DBAC3,
                    0x6D5B5A1E,0x9D2C4EAD,0x6C8BF73E,0x9F5A6F55,0x6CC3F0CC,0x9F2F0BB6,0x6D4FB039,0x9C2A3CA4,
                    0x6D85C0DD,0x9E06C49E,0x6C68E6E7,0x9F061D8F,0x6D08D414,0x9E485ED1,0x6C4AA1EA,0x9F09EB88,
                    0x6D4AC7A5,0x9D42B51E,0x6D0B4F07,0x9F32C5C2,0x6CCD9F21,0x9F616ED1,0x6D01B9F3,0x9F4BCE8E,
                    0x6D061A26,0x9F1B6E7C,0x6C7AC1A8,0x9F5851F3,0x6C9C1A8E,0x9E5A03EB,0x6D7BBED3,0x9F1BCB52,
                    0x6C5FCB6F,0x9F09F364,0x6C6F66C1,0x9E7CF2C2,0x6CCB9CD4,0x9F46C0B7,0x6D0BD6C0,0x9F5B2C4D,
                    0x6CCE12ED,0x9F55CBE5,0x6D62D676,0x9F5F4854,0x6C9D367C,0x9F67E8C3,0x6D5E61A0,0x9E2E468D,
                };
                const uint32_t* table = d.is_sub ? crc32c_poly : crc32_poly;
                int sz = d.imm;  // 0=B, 1=H, 2=W, 3=X
                if (sz == 0) {
                    crc = table[(crc ^ (data & 0xFF)) & 0xFF] ^ (crc >> 8);
                } else if (sz == 1) {
                    crc = table[(crc ^ (data & 0xFF)) & 0xFF] ^ (crc >> 8);
                    crc = table[(crc ^ ((data >> 8) & 0xFF)) & 0xFF] ^ (crc >> 8);
                } else if (sz == 2) {
                    for (int i = 0; i < 4; i++)
                        crc = table[(crc ^ ((data >> (i*8)) & 0xFF)) & 0xFF] ^ (crc >> 8);
                } else {  // sz == 3 (doubleword)
                    for (int i = 0; i < 8; i++)
                        crc = table[(crc ^ ((data >> (i*8)) & 0xFF)) & 0xFF] ^ (crc >> 8);
                }
                if (d.rd != 31) cpu.regs[d.rd] = static_cast<uint64_t>(crc);
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
                        // product to unsigned __int128 AFTER multiplying,
                        // which truncated the result to 64 bits before
                        // widening. This made SMULH always return 0 (or
                        // sign-extended garbage) for the high 64 bits,
                        // breaking glibc's __offtime which uses SMULH for
                        // division-by-constant optimization. The fix: cast
                        // each operand to __int128 BEFORE multiplying, so
                        // the product is a full 128-bit signed value.
                        __int128 prod = (static_cast<__int128>(static_cast<int64_t>(a)))
                                      * (static_cast<__int128>(static_cast<int64_t>(b)));
                        res = static_cast<uint64_t>(static_cast<unsigned __int128>(prod) >> 64);
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
                // For stores (Rt==Rn legal), capture the operands BEFORE
                // writeback — otherwise STP X0, X1, [X0, #16]! stores the
                // new pointer. The IR path reads operands first, so this
                // keeps interpreter/JIT in agreement.
                uint64_t s1 = 0, s2 = 0;
                if (!d.is_load && !d.is_vec) {
                    s1 = (d.rt  == 31) ? 0 : cpu.regs[d.rt];
                    s2 = (d.rt2 == 31) ? 0 : cpu.regs[d.rt2];
                }
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
                    // Vector STP/LDP. Each register transfer is `esize` bytes:
                    //   - esize=4 (S-form): 4 bytes (low 32 bits of v_lo)
                    //   - esize=8 (D-form): 8 bytes (all of v_lo)
                    //   - esize=16 (Q-form): 16 bytes (v_lo + v_hi)
                    //
                    //   mem_.write(addr, &v_lo[rt], esize, ...)
                    // with esize=16, which read 16 bytes from the 8-byte
                    // v_lo[rt] field — a buffer overread that wrote v_lo[rt]
                    // (8 bytes) + v_lo[rt+1] (8 garbage bytes from the NEXT
                    // register). The subsequent hi-half write fixed bytes
                    // 8-15, but the 16-byte overread was still UB and could
                    // corrupt data if v_lo[rt+1] was in a different thread's
                    // context (it isn't, but the pattern is still wrong).
                    //
                    // Fix: for Q-form stores, write v_lo (8 bytes) then v_hi
                    // (8 bytes) separately. For loads, read lo and hi
                    // separately. For S/D-form, use esize (4 or 8 bytes)
                    // which is <= sizeof(v_lo) and safe.
                    if (d.is_load) {
                        if (esize <= 8) {
                            // S/D-form: read esize bytes into v_lo, zero v_hi.
                            uint64_t lo1 = 0, lo2 = 0;
                            mem_.read(addr, &lo1, esize, pcache);
                            mem_.read(addr + esize, &lo2, esize, pcache);
                            cpu.v_lo[d.rt] = lo1;
                            cpu.v_hi[d.rt] = 0;
                            cpu.v_lo[d.rt2] = lo2;
                            cpu.v_hi[d.rt2] = 0;
                        } else {
                            // Q-form (esize=16): read 16 bytes = lo + hi.
                            uint64_t lo1, hi1, lo2, hi2;
                            mem_.read(addr, &lo1, 8, pcache);
                            mem_.read(addr + 8, &hi1, 8, pcache);
                            mem_.read(addr + 16, &lo2, 8, pcache);
                            mem_.read(addr + 24, &hi2, 8, pcache);
                            cpu.v_lo[d.rt] = lo1;
                            cpu.v_hi[d.rt] = hi1;
                            cpu.v_lo[d.rt2] = lo2;
                            cpu.v_hi[d.rt2] = hi2;
                        }
                    } else {
                        if (esize <= 8) {
                            // S/D-form: write esize bytes from v_lo.
                            mem_.write(addr, &cpu.v_lo[d.rt], esize, pcache);
                            mem_.write(addr + esize, &cpu.v_lo[d.rt2], esize, pcache);
                        } else {
                            // Q-form (esize=16): write v_lo (8 bytes) then
                            // v_hi (8 bytes) for each register.
                            mem_.write(addr, &cpu.v_lo[d.rt], 8, pcache);
                            mem_.write(addr + 8, &cpu.v_hi[d.rt], 8, pcache);
                            mem_.write(addr + 16, &cpu.v_lo[d.rt2], 8, pcache);
                            mem_.write(addr + 24, &cpu.v_hi[d.rt2], 8, pcache);
                        }
                    }
                } else {
                    if (d.is_load) {
                        uint64_t v1 = 0, v2 = 0;
                        mem_.read(addr, &v1, esize, pcache);
                        mem_.read(addr + esize, &v2, esize, pcache);
                        if (d.rt  != 31) cpu.regs[d.rt]  = v1;
                        if (d.rt2 != 31) cpu.regs[d.rt2] = v2;
                    } else {
                        mem_.write(addr, &s1, esize, pcache);
                        mem_.write(addr + esize, &s2, esize, pcache);
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
                        static bool vdbg_ = dbg().ld2;
                        if (vdbg_ && nbytes == 16) {
                            fprintf(stderr, "[STRQ-DBG] addr=0x%llx rt=%d bytes:",
                                    (unsigned long long)addr, d.rt);
                            for (int k = 0; k < 16; k++) {
                                fprintf(stderr, " %02x", (k < 8)
                                    ? (unsigned char)((lo >> (k*8)) & 0xFF)
                                    : (unsigned char)((hi >> ((k-8)*8)) & 0xFF));
                            }
                            fprintf(stderr, "\n");
                        }
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
                // For stores, capture the operand BEFORE writeback so
                // STR X0, [X0], #8 stores the original value (matches IR).
                uint64_t sv = 0;
                if (!d.is_load && d.rt != 31) sv = cpu.regs[d.rt];
                if (d.is_load) {
                    uint64_t v = 0;
                    mem_.read(addr, &v, width_bytes, pcache);
                    if (d.rt != 31) {
                        if (opc & 2) v = sign_extend(v, width_bytes * 8);
                        cpu.regs[d.rt] = v;
                    }
                } else {
                    uint64_t v = (d.rt == 31) ? 0 : sv;
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
