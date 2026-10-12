#include "FrostJIT/CodeGen/Native/NativeSIMD.hpp"
#include "System/Core/CPU.hpp"
#include "Interpreter/FloatSIMD/FloatCommon.hpp"
#include <cmath>
#include <cstring>
namespace arm64emu {
NativeSimdKind classify_native_simd(uint32_t w) {
    using K = NativeSimdKind;
    uint32_t b = w & 0xBFE0FC00;
    if (b == 0x2E601C00 || b == 0x2EA01C00 || b == 0x2EE01C00) return K::Select;
    unsigned imm5 = (w >> 16) & 31;
    if (b == 0x0E000400 && imm5 && imm5 != 16 &&
        ((imm5 & 7) || (w & (1u << 30)))) return K::Dup;
    if ((w & 0xFFE0FC00) == 0x4E001C00 && imm5 && imm5 != 16) return K::Insert;
    if ((w & 0xBFE08400) == 0x2E000000 && (((w >> 11) & 15) < ((w & (1u<<30)) ? 16u : 8u))) return K::Extract;
    unsigned h = (w >> 19) & 15;
    if ((w & 0xBF00FC00) == 0x0F008400 && h && h < 8) return K::NarrowShift;
    if ((w & 0x9F00FC00) == 0x0F00A400 && h && h < 8) return K::WidenShift;
    if ((w & 0xBF3FFC00) == 0x2E213800 && ((w >> 22) & 3) < 3) return K::WidenHigh;
    if ((w & 0xBFFFFC00) == 0x0E616800) return K::NarrowFp;
    if ((w & 0xBFFFFC00) == 0x0E617800) return K::WidenFp;
    if ((w & 0xBF20FC00) == 0x2E20E400 || (w & 0xBF20FC00) == 0x0E20E400) return K::CompareFp;
    // FP vector forms with sz=1 need Q=1 (.1d is reserved).
    const bool fp_lanes = !((w >> 22) & 1) || ((w >> 30) & 1);
    const uint32_t fz = w & 0xBFBFFC00;  // FCMGT/FCMEQ/FCMLT/FCMGE/FCMLE #0
    if (fp_lanes && (fz == 0x0EA0C800 || fz == 0x0EA0D800 || fz == 0x0EA0E800 ||
                     fz == 0x2EA0C800 || fz == 0x2EA0D800)) return K::CompareZeroFp;
    if ((w & 0xBF3FFC00) == 0x0E209800) return K::CompareZeroInt;
    if ((w & 0x9F800C00) == 0x0F000400 && !h && ((w >> 12) & 15) == 15) return K::ImmediateFp;
    if ((w & 0x9F20FC00) == 0x0E204400) return K::VariableShift;
    if (fp_lanes && arm_frint_vector_mode(w) >= 0) return K::RoundFp;
    if ((w & 0xBFFFFC00) == 0x0E61D800 || (w & 0xBFFFFC00) == 0x0EE1B800) return K::Convert64;
    if ((w & 0xFFE0FC00) == 0x5EE08400) return K::ScalarAdd;
    if ((w & 0xBF3FFC00) == 0x0E31B800 && ((w >> 22) & 3) < 3) return K::ReduceAdd;
    if ((w & 0xBF3FFC00) == 0x0E200800 && ((w >> 22) & 3) < 3) return K::Reverse64;
    if ((w & 0xBF20FC00) == 0x0E209400 && ((w >> 22) & 3) < 3) return K::MultiplyAdd;
    if ((w & 0xBFC0F400) == 0x0F801000) return K::IndexedFma; // FP32 FMLA by element
    // Across lanes: 64-bit elements and 32-bit lanes without Q are reserved.
    const unsigned sz = (w >> 22) & 3;
    const bool q = (w >> 30) & 1;
    const bool across_size = sz < 2 || (sz == 2 && q);
    if ((w & 0x9F3FFC00) == 0x0E303800 && across_size) return K::AcrossLong;   // S/UADDLV
    if ((w & 0x9F3EFC00) == 0x0E30A800 && across_size) return K::AcrossMinMax; // S/U MAX/MIN V
    // Widening three-different ops (8-bit to 32-bit sources).
    const uint32_t three = w & 0x9F20FC00;
    if (sz < 3 && (three == 0x0E200000 || three == 0x0E202000 || three == 0x0E201000 ||
                   three == 0x0E203000 || three == 0x0E208000 || three == 0x0E20A000 ||
                   three == 0x0E20C000)) return K::WidenThree;  // ADDL SUBL ADDW SUBW MLAL MLSL MULL
    const uint32_t pair = w & 0x9F3FFC00;
    if (sz < 3 && (pair == 0x0E202800 || pair == 0x0E206800)) return K::PairLong;  // ADDLP ADALP
    const uint32_t fpair = w & 0xBFA0FC00;  // FMAXP FMINP FMAXNMP FMINNMP
    if (fp_lanes && (fpair == 0x2E20F400 || fpair == 0x2EA0F400 ||
                     fpair == 0x2E20C400 || fpair == 0x2EA0C400)) return K::PairFp;
    const uint32_t half = w & 0xBFFFFC00;   // FCVTL/FCVTN with half lanes
    if (half == 0x0E217800 || half == 0x0E216800) return K::HalfFp;
    return K::None;
}
namespace {
struct Vec { uint8_t bytes[16]{}; };
Vec read(const CPU* cpu, unsigned reg) {
    Vec v; std::memcpy(v.bytes, &cpu->v_lo[reg], 8);
    std::memcpy(v.bytes+8, &cpu->v_hi[reg], 8); return v;
}
void write(CPU* cpu, unsigned reg, const Vec& v) {
    std::memcpy(&cpu->v_lo[reg], v.bytes, 8); std::memcpy(&cpu->v_hi[reg], v.bytes+8, 8);
}
uint64_t get(const Vec& v, unsigned lane, unsigned size) {
    // Lanes are at most 8 bytes; the cap tells the compiler so.
    uint64_t x=0; std::memcpy(&x, v.bytes+lane*size, size < 8 ? size : 8); return x;
}
void put(Vec& v, unsigned lane, unsigned size, uint64_t x) {
    std::memcpy(v.bytes+lane*size, &x, size < 8 ? size : 8);
}
int64_t signed_lane(uint64_t x, unsigned bits) {
    return bits == 64 ? static_cast<int64_t>(x) : static_cast<int64_t>(x << (64-bits)) >> (64-bits);
}
template<class T> T fp(const Vec& v, unsigned lane) {
    T x; std::memcpy(&x, v.bytes+lane*sizeof(T), sizeof(T)); return x;
}
template<class T> void put_fp(Vec& v, unsigned lane, T x) { std::memcpy(v.bytes+lane*sizeof(T), &x, sizeof(T)); }
template<NativeSimdKind K> void run(CPU* cpu, uint32_t w) {
    unsigned rd=w&31, rn=(w>>5)&31, rm=(w>>16)&31;
    bool q=(w>>30)&1, u=(w>>29)&1;
    unsigned size=1u<<((w>>22)&3), bytes=q?16:8;
    Vec n=read(cpu,rn), m=read(cpu,rm), d=read(cpu,rd), out;
    if constexpr (K==NativeSimdKind::Select) {
        unsigned which=(w>>22)&3;
        for(unsigned i=0;i<bytes;i++) {
            if(which==1) out.bytes[i]=(n.bytes[i]&d.bytes[i])|(m.bytes[i]&~d.bytes[i]);
            else if(which==2) out.bytes[i]=(n.bytes[i]&m.bytes[i])|(d.bytes[i]&~m.bytes[i]);
            else out.bytes[i]=(n.bytes[i]&~m.bytes[i])|(d.bytes[i]&m.bytes[i]);
        }
    } else if constexpr(K==NativeSimdKind::Dup || K==NativeSimdKind::Insert) {
        unsigned imm=(w>>16)&31, shift=__builtin_ctz(imm), esize=1u<<shift, lane=imm>>(shift+1);
        if constexpr(K==NativeSimdKind::Insert) { out=d; put(out,lane,esize,rn==31?0:cpu->regs[rn]); }
        else { uint64_t val=get(n,lane,esize); for(unsigned i=0;i<bytes/esize;i++) put(out,i,esize,val); }
    } else if constexpr(K==NativeSimdKind::Extract) {
        unsigned off=(w>>11)&15;
        for(unsigned i=0;i<bytes;i++) out.bytes[i]=(i+off<bytes)?n.bytes[i+off]:m.bytes[i+off-bytes];
    } else if constexpr(K==NativeSimdKind::NarrowShift) {
        unsigned imm=(w>>16)&127, h=(w>>19)&15;
        unsigned esize=1u<<(31-__builtin_clz(h)), shift=esize*16-imm;
        for(unsigned i=0;i<8/esize;i++) put(out,i,esize,get(n,i,esize*2)>>shift);
        if(q) { std::memcpy(out.bytes+8,out.bytes,8); std::memcpy(out.bytes,d.bytes,8); }
    } else if constexpr(K==NativeSimdKind::WidenShift || K==NativeSimdKind::WidenHigh) {
        unsigned esize, shift;
        if constexpr(K==NativeSimdKind::WidenShift) {
            unsigned h=(w>>19)&15; esize=1u<<(31-__builtin_clz(h)); shift=((w>>16)&127)-esize*8;
        } else { esize=size; shift=size*8; }
        unsigned first=q?8/esize:0;
        for(unsigned i=0;i<8/esize;i++) {
            uint64_t value=get(n,first+i,esize);
            if constexpr(K==NativeSimdKind::WidenShift) if(!u) value=static_cast<uint64_t>(signed_lane(value,esize*8));
            put(out,i,esize*2,value<<shift);
        }
    } else if constexpr(K==NativeSimdKind::NarrowFp) {
        put_fp(out,0,static_cast<float>(fp<double>(n,0))); put_fp(out,1,static_cast<float>(fp<double>(n,1)));
        if(q) { std::memcpy(out.bytes+8,out.bytes,8); std::memcpy(out.bytes,d.bytes,8); }
    } else if constexpr(K==NativeSimdKind::WidenFp) {
        put_fp(out,0,static_cast<double>(fp<float>(n,q?2:0))); put_fp(out,1,static_cast<double>(fp<float>(n,q?3:1)));
    } else if constexpr(K==NativeSimdKind::CompareFp || K==NativeSimdKind::CompareZeroFp) {
        unsigned esize=(w&(1u<<22))?8:4;
        for(unsigned i=0;i<bytes/esize;i++) {
            double a=esize==8?fp<double>(n,i):fp<float>(n,i);
            double b=esize==8?fp<double>(m,i):fp<float>(m,i);
            bool match;
            if constexpr(K==NativeSimdKind::CompareZeroFp) {
                // bits[13:12]: 0 GT/GE, 1 EQ/LE, 2 LT; U selects GE/LE.
                const unsigned c=(w>>12)&3;
                match = c==2 ? a<0 : c==1 ? (u ? a<=0 : a==0) : (u ? a>=0 : a>0);
            }
            else if(!u) match=a==b;
            else match=(w&(1u<<23))?a>b:a>=b;
            put(out,i,esize,match?~uint64_t(0):0);
        }
    } else if constexpr(K==NativeSimdKind::CompareZeroInt) {
        for(unsigned i=0;i<bytes/size;i++) put(out,i,size,get(n,i,size)==0?~uint64_t(0):0);
    } else if constexpr(K==NativeSimdKind::ImmediateFp) {
        unsigned imm=(((w>>16)&7)<<5)|((w>>5)&31);
        uint64_t value;
        if(u) value=(uint64_t(imm&63)<<48)|(uint64_t(imm&128)<<56)|((imm&64)?0x3FC0000000000000ULL:0x4000000000000000ULL);
        else value=(uint64_t(imm&63)<<19)|(uint64_t(imm&128)<<24)|((imm&64)?0x3E000000ULL:0x40000000ULL);
        unsigned esize=u?8:4; for(unsigned i=0;i<bytes/esize;i++) put(out,i,esize,value);
    } else if constexpr(K==NativeSimdKind::VariableShift) {
        for(unsigned i=0;i<bytes/size;i++) {
            int shift=static_cast<int8_t>(get(m,i,size)&255); unsigned bits=size*8;
            uint64_t a=get(n,i,size), value;
            if(shift>=0) value=unsigned(shift)>=bits?0:a<<shift;
            else if(unsigned(-shift)>=bits) value=u?0:static_cast<uint64_t>(signed_lane(a,bits)>>63);
            else value=u?a>>(-shift):static_cast<uint64_t>(signed_lane(a,bits)>>(-shift));
            put(out,i,size,value);
        }
    } else if constexpr(K==NativeSimdKind::RoundFp) {
        unsigned esize=(w&(1u<<22))?8:4;
        const int mode=arm_frint_vector_mode(w); const bool dn=(cpu->fpcr>>25)&1;
        for(unsigned i=0;i<bytes/esize;i++) {
            if(esize==8) put_fp(out,i,arm_frint(mode,fp<double>(n,i),dn));
            else put_fp(out,i,arm_frint(mode,fp<float>(n,i),dn));
        }
    } else if constexpr(K==NativeSimdKind::Convert64) {
        bool to_int=(w&(1u<<23))!=0;
        for(unsigned i=0;i<bytes/8;i++) {
            if(!to_int) put_fp(out,i,static_cast<double>(static_cast<int64_t>(get(n,i,8))));
            else {
                double x=fp<double>(n,i); int64_t y;
                if(std::isnan(x)) y=0;
                else if(x>=0x1p63) y=INT64_MAX;
                else if(x<=-0x1p63) y=INT64_MIN;
                else y=static_cast<int64_t>(x);
                put(out,i,8,static_cast<uint64_t>(y));
            }
        }
    } else if constexpr(K==NativeSimdKind::ScalarAdd) { put(out,0,8,get(n,0,8)+get(m,0,8)); }
    else if constexpr(K==NativeSimdKind::ReduceAdd) {
        uint64_t sum=0; for(unsigned i=0;i<bytes/size;i++) sum+=get(n,i,size); put(out,0,size,sum);
    } else if constexpr(K==NativeSimdKind::Reverse64) {
        unsigned count=8/size;
        for(unsigned i=0;i<bytes/size;i++) put(out,i,size,get(n,(i/count)*count+count-1-i%count,size));
    } else if constexpr(K==NativeSimdKind::MultiplyAdd) {
        for(unsigned i=0;i<bytes/size;i++) put(out,i,size,get(d,i,size)+get(n,i,size)*get(m,i,size));
    } else if constexpr(K==NativeSimdKind::IndexedFma) {
        unsigned lane=((w>>21)&1)|(((w>>11)&1)<<1); float value=fp<float>(m,lane);
        for(unsigned i=0;i<bytes/4;i++) put_fp(out,i,arm_fp_muladd(fp<float>(d,i),fp<float>(n,i),value));
    } else if constexpr(K==NativeSimdKind::AcrossLong || K==NativeSimdKind::AcrossMinMax) {
        // Element `size`; SADDLV/UADDLV write a 2*size sum, MAX/MINV a size lane.
        auto ext = [&](unsigned i) {
            uint64_t x = get(n, i, size);
            return u ? static_cast<int64_t>(x) : signed_lane(x, size * 8);
        };
        int64_t acc = ext(0);
        const bool is_min = (w >> 16) & 1;
        for (unsigned i = 1; i < bytes / size; i++) {
            const int64_t x = ext(i);
            if constexpr (K==NativeSimdKind::AcrossLong) acc += x;
            else {
                const bool less = u ? static_cast<uint64_t>(x) < static_cast<uint64_t>(acc) : x < acc;
                const bool greater = u ? static_cast<uint64_t>(x) > static_cast<uint64_t>(acc) : x > acc;
                if (is_min ? less : greater) acc = x;
            }
        }
        put(out, 0, K==NativeSimdKind::AcrossLong ? size * 2 : size, static_cast<uint64_t>(acc));
    } else if constexpr(K==NativeSimdKind::WidenThree) {
        // Narrow lanes come from the upper halves for the "2" (Q=1) forms;
        // ADDW/SUBW take Vn at full width.
        const unsigned op = (w >> 12) & 15, count = 8 / size, half = q ? count : 0;
        auto ext = [&](const Vec& v, unsigned i) {
            uint64_t x = get(v, half + i, size);
            return u ? x : static_cast<uint64_t>(signed_lane(x, size * 8));
        };
        for (unsigned i = 0; i < count; i++) {
            const uint64_t a = ext(n, i), b = ext(m, i), wide = get(n, i, size * 2);
            uint64_t r;
            switch (op) {
                case 0x0: r = a + b; break;                        // ADDL
                case 0x2: r = a - b; break;                        // SUBL
                case 0x1: r = wide + b; break;                     // ADDW
                case 0x3: r = wide - b; break;                     // SUBW
                case 0x8: r = get(d, i, size * 2) + a * b; break;  // MLAL
                case 0xA: r = get(d, i, size * 2) - a * b; break;  // MLSL
                default:  r = a * b; break;                        // MULL
            }
            put(out, i, size * 2, r);
        }
    } else if constexpr(K==NativeSimdKind::PairLong) {
        // ADDLP sums adjacent pairs into double-width lanes; ADALP adds Vd.
        const bool acc = (w >> 14) & 1;
        for (unsigned i = 0; i < bytes / (size * 2); i++) {
            uint64_t a = get(n, 2 * i, size), b = get(n, 2 * i + 1, size);
            if (!u) {
                a = static_cast<uint64_t>(signed_lane(a, size * 8));
                b = static_cast<uint64_t>(signed_lane(b, size * 8));
            }
            put(out, i, size * 2, a + b + (acc ? get(d, i, size * 2) : 0));
        }
    } else if constexpr(K==NativeSimdKind::PairFp) {
        // Lanes come from Vm:Vn pairs: Vn's pairs fill the low results.
        const ArmFpOp op = (w & 0x2000) ? ((w >> 23) & 1 ? ArmFpOp::Min : ArmFpOp::Max)
                                        : ((w >> 23) & 1 ? ArmFpOp::MinNum : ArmFpOp::MaxNum);
        const unsigned esize = (w >> 22) & 1 ? 8 : 4, lanes = bytes / esize, half = lanes / 2;
        for (unsigned e = 0; e < lanes; e++) {
            const Vec& src = e < half ? n : m;
            const unsigned idx = 2 * (e < half ? e : e - half);
            if (esize == 8) put_fp(out, e, arm_fp_binop(op, fp<double>(src, idx), fp<double>(src, idx + 1)));
            else put_fp(out, e, arm_fp_binop(op, fp<float>(src, idx), fp<float>(src, idx + 1)));
        }
    } else if constexpr(K==NativeSimdKind::HalfFp) {
        // FCVTL widens the selected half's four halves; FCVTN narrows four
        // singles into the low half (Q=0, top zeroed) or high half (Q=1).
        const bool dn = (cpu->fpcr >> 25) & 1;
        if ((w >> 12) & 1) {  // opcode 0x17 FCVTL, 0x16 FCVTN
            for (unsigned i = 0; i < 4; i++)
                put_fp(out, i, h2f(static_cast<uint16_t>(get(n, (q ? 4 : 0) + i, 2)), dn));
        } else {
            if (q) std::memcpy(out.bytes, d.bytes, 8);
            for (unsigned i = 0; i < 4; i++) put(out, (q ? 4 : 0) + i, 2, f2h(fp<float>(n, i), dn));
        }
    }
    write(cpu,rd,out);
}
}
NativeSimdFn native_simd_helper(NativeSimdKind k) {
#define CASE(kind) case NativeSimdKind::kind: return &run<NativeSimdKind::kind>
    switch(k) {
        CASE(Select); CASE(Dup); CASE(Insert); CASE(Extract); CASE(NarrowShift);
        CASE(WidenShift); CASE(WidenHigh); CASE(NarrowFp); CASE(WidenFp);
        CASE(CompareFp); CASE(CompareZeroFp); CASE(CompareZeroInt); CASE(ImmediateFp);
        CASE(VariableShift); CASE(RoundFp); CASE(Convert64); CASE(ScalarAdd);
        CASE(ReduceAdd); CASE(Reverse64); CASE(MultiplyAdd); CASE(IndexedFma);
        CASE(AcrossLong); CASE(AcrossMinMax); CASE(WidenThree); CASE(PairLong);
        CASE(PairFp); CASE(HalfFp);
        default: return nullptr;
    }
#undef CASE
}
}
