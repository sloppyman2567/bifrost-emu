#include "jit/native_simd.hpp"
#include "core/cpu.h"
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
    if ((w & 0xBFBFFC00) == 0x0EA0E800) return K::CompareZeroFp;
    if ((w & 0xBF3FFC00) == 0x0E209800) return K::CompareZeroInt;
    if ((w & 0x9F800C00) == 0x0F000400 && !h && ((w >> 12) & 15) == 15) return K::ImmediateFp;
    if ((w & 0x9F20FC00) == 0x0E204400) return K::VariableShift;
    if ((w & 0xBFBFFC00) == 0x0E219800) return K::RoundFp;
    if ((w & 0xBFFFFC00) == 0x0E61D800 || (w & 0xBFFFFC00) == 0x0EE1B800) return K::Convert64;
    if ((w & 0xFFE0FC00) == 0x5EE08400) return K::ScalarAdd;
    if ((w & 0xBF3FFC00) == 0x0E31B800 && ((w >> 22) & 3) < 3) return K::ReduceAdd;
    if ((w & 0xBF3FFC00) == 0x0E200800 && ((w >> 22) & 3) < 3) return K::Reverse64;
    if ((w & 0xBF20FC00) == 0x0E209400 && ((w >> 22) & 3) < 3) return K::MultiplyAdd;
    if ((w & 0xBFC0F400) == 0x0F801000) return K::IndexedFma; // FP32 FMLA by element
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
    uint64_t x=0; std::memcpy(&x, v.bytes+lane*size, size); return x;
}
void put(Vec& v, unsigned lane, unsigned size, uint64_t x) {
    std::memcpy(v.bytes+lane*size, &x, size);
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
            if constexpr(K==NativeSimdKind::CompareZeroFp) match=a<0;
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
        for(unsigned i=0;i<bytes/esize;i++) {
            if(esize==8) put_fp(out,i,std::floor(fp<double>(n,i)));
            else put_fp(out,i,std::floor(fp<float>(n,i)));
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
        for(unsigned i=0;i<bytes/4;i++) put_fp(out,i,std::fma(fp<float>(n,i),value,fp<float>(d,i)));
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
        default: return nullptr;
    }
#undef CASE
}
}
