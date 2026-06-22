// ir/ir_lower.cpp — SWAR lowering of RBIT/REV16/REV32 into IR primitives.
//
// These decompose the ARM64 bit-reversal / byte-swap ops into primitive
// IR ops (AND, OR, SHL, SHR) that the JIT already compiles natively.
// This replaces the CALL_INTERP fallback that the JIT used to take for
// these ops, and lets the optimizer fold/propagate when the source is a
// known constant.
//
// All helpers take an input vreg and return a fresh vreg holding the
// transformed value. The 32-bit variants assume the caller has already
// restricted the input to 32 bits (via ZEXT) and will ZEXT the result
// again to clear the high 32 bits.
#include "ir/ir.h"

namespace arm64emu {

// v = ((v >> n) & mask) | ((v & mask) << n)
// Used for swap-with-mask patterns. Each call is 4 IR ops.
uint16_t swar_swap(IRBlock& b, uint16_t v, uint64_t mask, int n) {
    uint16_t mask_v = load_imm(b, mask);
    uint16_t n_v    = load_imm(b, (uint64_t)n);
    // hi = (v & mask) << n
    uint16_t kept   = g_alloc.alloc();
    emit(b, IROp::AND, kept, v, mask_v);
    uint16_t hi     = g_alloc.alloc();
    emit(b, IROp::SHL, hi, kept, n_v);
    // lo = (v >> n) & mask
    uint16_t shr    = g_alloc.alloc();
    emit(b, IROp::SHR, shr, v, n_v);
    uint16_t lo     = g_alloc.alloc();
    emit(b, IROp::AND, lo, shr, mask_v);
    // out = hi | lo
    uint16_t out    = g_alloc.alloc();
    emit(b, IROp::OR, out, hi, lo);
    return out;
}

// 64-bit bit-reversal via 6 SWAR stages:
//   swap bits 1<>0, 3<>2, ..., 63<>62  (mask=0x5555..., n=1)
//   swap pairs  3<>1, 2<>0, ..., 63<>61 (mask=0x3333..., n=2)
//   swap nibbles 7<>4, 6<>5, ..., 63<>60 (mask=0x0F0F..., n=4)
//   swap bytes within 16-bit halfwords   (mask=0x00FF..., n=8)
//   swap 16-bit halfwords within 32-bit words (mask=0x0000FFFF..., n=16)
//   swap 32-bit words                     (n=32, no mask needed)
uint16_t rbit64_ir(IRBlock& b, uint16_t v) {
    v = swar_swap(b, v, 0x5555555555555555ULL,  1);
    v = swar_swap(b, v, 0x3333333333333333ULL,  2);
    v = swar_swap(b, v, 0x0F0F0F0F0F0F0F0FULL,  4);
    v = swar_swap(b, v, 0x00FF00FF00FF00FFULL,  8);
    v = swar_swap(b, v, 0x0000FFFF0000FFFFULL, 16);
    // Final 32-bit swap: out = (v << 32) | (v >> 32). No mask needed.
    uint16_t n32 = load_imm(b, 32);
    uint16_t hi  = g_alloc.alloc(); emit(b, IROp::SHL, hi, v, n32);
    uint16_t lo  = g_alloc.alloc(); emit(b, IROp::SHR, lo, v, n32);
    uint16_t out = g_alloc.alloc(); emit(b, IROp::OR,  out, hi, lo);
    return out;
}

// 32-bit bit-reversal: same idea, 5 stages (no final 32-bit swap).
uint16_t rbit32_ir(IRBlock& b, uint16_t v) {
    v = swar_swap(b, v, 0x55555555ULL,  1);
    v = swar_swap(b, v, 0x33333333ULL,  2);
    v = swar_swap(b, v, 0x0F0F0F0FULL,  4);
    v = swar_swap(b, v, 0x00FF00FFULL,  8);
    v = swar_swap(b, v, 0x0000FFFFULL, 16);
    return v;
}

// REV16 (64-bit): swap bytes within each 16-bit halfword.
//   mask=0x00FF00FF00FF00FF, n=8
uint16_t rev16_64_ir(IRBlock& b, uint16_t v) {
    return swar_swap(b, v, 0x00FF00FF00FF00FFULL, 8);
}

// REV32 (64-bit): swap bytes within each 32-bit word.
//   = REV16 followed by swap of 16-bit halves within each 32-bit word.
uint16_t rev32_64_ir(IRBlock& b, uint16_t v) {
    v = swar_swap(b, v, 0x00FF00FF00FF00FFULL, 8);
    v = swar_swap(b, v, 0x0000FFFF0000FFFFULL, 16);
    return v;
}

} // namespace arm64emu
