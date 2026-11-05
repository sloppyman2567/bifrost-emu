// A head earns a trial by guest work per second, not by lifetime entries.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include "System/Core/CPU.hpp"
#include <cstdio>
#include <memory>
#include <unistd.h>
#include <vector>
using namespace arm64emu;
namespace arm64emu {
struct Tier2TestAccess {
    static size_t trials(FrostJIT& j) { return j.tier2_trials_.size(); }
    static bool settled(FrostJIT& j,uint64_t pc) { return j.tier2_settled_.count(pc)!=0; }
};
}
constexpr uint64_t SLOW=0x10000,FAST=0x11000,STOP=0x20000;
int main() {
    auto e=std::make_unique<Emulator>();
    e->mem().map_range(SLOW,0x2000); e->mem().mprotect_guest(SLOW,0x2000,7);
    // Eight blocks of add x1,x1,#1; b next, then ret: a path that gets a trial
    // without the gate.
    std::vector<uint32_t> slow;
    for(unsigned k=0;k<8;++k) { slow.push_back(0x91000421); slow.push_back(0x14000001); }
    slow.push_back(0xd65f03c0);
    // add x1,x1,#1; b next; sub x0,x0,#1; cbnz x0,head; ret
    const uint32_t fast[]={0x91000421,0x14000001,0xd1000400,0xb5ffffa0,0xd65f03c0};
    e->mem().write(SLOW,slow.data(),slow.size()*4); e->mem().write(FAST,fast,sizeof(fast));
    e->enable_jit();
    auto* j=e->jit();
    auto run=[&](uint64_t pc,uint64_t n) {
        CPU c; c.pc=pc; c.regs[0]=n; c.regs[30]=STOP; c.jit_stop_pc=STOP;
        while(c.pc!=STOP) c.pc=j->run_block(c,*e);
    };
    // Past the old lifetime bar many times over, but far below the rate.
    for(unsigned k=0;k<5000;++k) { run(SLOW,0); usleep(100); }
    if(Tier2TestAccess::trials(*j) || !Tier2TestAccess::settled(*j,SLOW)) {
        std::fprintf(stderr,"slow head: trials=%zu settled=%d\n",Tier2TestAccess::trials(*j),
            (int)Tier2TestAccess::settled(*j,SLOW));
        return 1;
    }
    for(unsigned k=0;k<200 && !Tier2TestAccess::trials(*j);++k) run(FAST,50000);
    if(!Tier2TestAccess::trials(*j)) { std::fputs("fast head never reached a trial\n",stderr); return 1; }
    std::puts("jit_tier2_rate: a slow head settles, a fast one reaches a trial PASS");
}
