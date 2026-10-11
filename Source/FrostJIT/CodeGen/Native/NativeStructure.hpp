#pragma once
#include <cstdint>
namespace arm64emu {
class Emulator;
class CPU;
struct NativeStructureParams {
    uint8_t first=0, base=0, offset_reg=31, size=1, count=1, structure=1, lane=0;
    bool load=true, q=true, single=false, replicate=false, post=false;
    uint64_t pack() const;
    static NativeStructureParams unpack(uint64_t bits);
};
inline uint64_t NativeStructureParams::pack() const {
    return uint64_t(first)|(uint64_t(base)<<5)|(uint64_t(offset_reg)<<10)|
           (uint64_t(size)<<15)|(uint64_t(count)<<19)|(uint64_t(structure)<<22)|
           (uint64_t(lane)<<25)|(uint64_t(load)<<29)|(uint64_t(q)<<30)|
           (uint64_t(single)<<31)|(uint64_t(replicate)<<32)|(uint64_t(post)<<33);
}
inline NativeStructureParams NativeStructureParams::unpack(uint64_t b) {
    NativeStructureParams p;
    p.first=b&31; p.base=(b>>5)&31; p.offset_reg=(b>>10)&31;
    p.size=(b>>15)&15; p.count=(b>>19)&7; p.structure=(b>>22)&7;
    p.lane=(b>>25)&15; p.load=(b>>29)&1; p.q=(b>>30)&1;
    p.single=(b>>31)&1; p.replicate=(b>>32)&1; p.post=(b>>33)&1; return p;
}
uint64_t native_structure(Emulator*, CPU*, uint64_t descriptor);
}
