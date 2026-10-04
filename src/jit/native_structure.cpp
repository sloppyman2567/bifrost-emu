#include "jit/native_structure.hpp"
#include "core/emulator.h"
#include "core/signal.h"
#include <cstring>
namespace arm64emu {
uint64_t native_structure(Emulator* emu, CPU* cpu, uint64_t bits) {
    const auto p=NativeStructureParams::unpack(bits);
    const uint64_t base=p.base==31?cpu->sp:cpu->regs[p.base];
    const unsigned bytes=p.q?16:8;
    const unsigned total=p.single?p.count*p.size:p.count*bytes;
    uint8_t memory[64]{}, regs[4][16]{};
    // Snapshot all aliased sources before committing any destination.
    for(unsigned i=0;i<p.count;i++) {
        unsigned r=(p.first+i)&31;
        std::memcpy(regs[i],&cpu->v_lo[r],8); std::memcpy(regs[i]+8,&cpu->v_hi[r],8);
    }
    try {
        emu->mem().check_access(base,total,p.load?Memory::GUEST_PROT_READ:Memory::GUEST_PROT_WRITE);
        if(p.load) emu->mem().read(base,memory,total,&cpu->page_cache);
        if(p.single) {
            for(unsigned i=0;i<p.count;i++) {
                if(p.replicate) {
                    std::memset(regs[i],0,16);
                    for(unsigned lane=0;lane<bytes/p.size;lane++)
                        std::memcpy(regs[i]+lane*p.size,memory+i*p.size,p.size);
                } else if(p.load) std::memcpy(regs[i]+p.lane*p.size,memory+i*p.size,p.size);
                else std::memcpy(memory+i*p.size,regs[i]+p.lane*p.size,p.size);
            }
        } else {
            for(unsigned i=0;i<p.count;i++) {
                for(unsigned lane=0;lane<bytes/p.size;lane++) {
                    unsigned offset=p.structure==1?i*bytes+lane*p.size:(lane*p.count+i)*p.size;
                    if(p.load) std::memcpy(regs[i]+lane*p.size,memory+offset,p.size);
                    else std::memcpy(memory+offset,regs[i]+lane*p.size,p.size);
                }
                if(p.load && !p.q) std::memset(regs[i]+8,0,8);
            }
        }
        if(p.load) for(unsigned i=0;i<p.count;i++) {
            unsigned r=(p.first+i)&31;
            std::memcpy(&cpu->v_lo[r],regs[i],8); std::memcpy(&cpu->v_hi[r],regs[i]+8,8);
        } else {
            Memory::note_interp_pc(cpu->pc);
            emu->mem().write(base,memory,total,&cpu->page_cache);
        }
        if(p.post) {
            // Read the writeback operand before changing an aliased base.
            uint64_t offset=p.offset_reg==31?total:(p.offset_reg==30?0:cpu->regs[p.offset_reg]);
            if(p.base==31) cpu->sp=base+offset; else cpu->regs[p.base]=base+offset;
        }
        cpu->pc+=4;
    } catch(UnmappedMemory& e) {
        deliver_signal(*emu,*cpu,emu->signals(),BIFROST_SIGSEGV,e.segv_code(),e.addr);
    }
    return cpu->pc;
}
}
