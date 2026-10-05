// Behavioral oracles for precise retirement, recoveries and concurrent phases.
#include "core/emulator.h"
#include "jit/frostjit.hpp"
#include "tier2_test_access.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
using namespace arm64emu;
constexpr uint64_t A=0x10000, B=0x10200, STOP=0x20000;
static bool run(Emulator& e, CPU& c, uint64_t pc, uint64_t bit, uint64_t n=4000) {
    c.pc=pc; c.regs[0]=n; c.regs[1]=0; c.regs[2]=bit;
    c.regs[30]=STOP; c.jit_stop_pc=STOP;
    unsigned dispatches=0;
    while(c.pc!=STOP && ++dispatches<100000) c.pc=e.jit()->run_block(c,e);
    return c.pc==STOP && c.regs[1]==n*(bit ? 1 : 100);
}
static bool scenario(bool mt, bool concurrent) {
    auto e=std::make_unique<Emulator>();
    e->mem().map_range(A,4096); e->mem().mprotect_guest(A,4096,7);
    std::vector<uint32_t> code(13,0xd503201f);
    code[0]=0xf100005f; code[1]=0xb5000102; // cmp; cbnz x2,hot
    code[2]=0x91019021; code[3]=0x14000007; // cold +=100; b join
    code[9]=0x91000421; code[10]=0xd1000400; // hot +=1; --n
    code[11]=0xb5fffea0; code[12]=0xd65f03c0; // cbnz n,head; ret
    for(auto pc:{A,B}) e->mem().write(pc,code.data(),code.size()*4);
    e->enable_jit(); auto* j=e->jit(); if(mt) j->enter_multithreaded();
    CPU a,b;
    if(!run(*e,a,A,1)||!run(*e,b,B,1)||j->tier2_regions.load()<2) return false;
    Tier2TestAccess::keep_candidates(*j);
    const auto neighbor=j->lookup_only(B);
    const auto retired=j->tier2_phase_retirements.load();
    // Isolated cold exits separated by recoveries must not retire a region.
    for(unsigned k=0;k<100;++k)
        if(!run(*e,a,A,0,1)||!run(*e,a,A,1,4)) return false;
    if(j->tier2_phase_retirements.load()!=retired) return false;
    const size_t prefix=j->code_buf_used();
    std::vector<uint8_t> bytes(j->code_buf(),j->code_buf()+prefix);
    if(concurrent) {
        std::atomic<unsigned> ready{0}, failures{0};
        std::atomic<bool> start{false};
        std::vector<std::thread> workers;
        for(unsigned k=0;k<8;++k) workers.emplace_back([&,k] {
            CPU c; ready.fetch_add(1,std::memory_order_release);
            while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
            if(!run(*e,c,k==7 ? B : A,k==7 ? 1 : 0,20000)) ++failures;
        });
        while(ready.load(std::memory_order_acquire)!=8) std::this_thread::yield();
        start.store(true,std::memory_order_release);
        for(auto& t:workers) t.join();
        if(failures.load()) return false;
    } else if(!run(*e,a,A,0)) return false;
    // Retirement must preserve a distinct region even on the same page.
    if(j->lookup_only(B)!=neighbor || j->tier2_phase_retirements.load()!=retired+1)
        return false;
    if(mt && std::memcmp(bytes.data(),j->code_buf(),prefix)) return false;
    auto path=j->collect_tier2_trace(*e,A);
    if(!path.fully_profiled || path.blocks[0].follows_taken) return false;
    return run(*e,b,B,1) && run(*e,a,A,0);
}
// A cached region's exit target lies on the next page. Rewriting that
// target must preserve the region but change the executed result.
static bool rewritten_exit(bool mt, bool transition=false) {
    auto e=std::make_unique<Emulator>();
    e->mem().map_range(A,8192); e->mem().mprotect_guest(A,8192,7);
    const uint32_t head=0x140003fd;
    const uint32_t body[]={0x91000421,0xd1000400,0xb5ff8020};
    const uint32_t tail[]={0x91000421,0xd65f03c0};
    e->mem().write(A,&head,4); e->mem().write(A+0xff4,body,sizeof(body));
    e->mem().write(A+4096,tail,sizeof(tail));
    e->enable_jit(); auto* j=e->jit(); if(mt) j->enter_multithreaded();
    j->lookup_or_translate(*e,A+4096); // target exists before region publication
    auto execute=[&](uint64_t want) {
        CPU c; c.pc=A; c.regs[0]=2000; c.regs[30]=STOP; c.jit_stop_pc=STOP;
        unsigned n=0;
        while(c.pc!=STOP && ++n<10000) c.pc=j->run_block(c,*e);
        return c.pc==STOP && c.regs[1]==want;
    };
    if(!execute(2001) || !j->tier2_regions.load()) return false;
    Tier2TestAccess::keep_candidates(*j);
    auto region=j->lookup_only(A);
    if(transition) j->enter_multithreaded();
    const uint32_t changed=0x91019021;
    e->mem().write(A+4096,&changed,4); j->invalidate_range(A+4096,4);
    if(transition) {
        // Simulate an acquired old ST function finishing after transition.
        // Its exit cell must return to dispatch rather than call stale code.
        CPU c; c.pc=A; c.regs[0]=2000; c.regs[30]=STOP; c.jit_stop_pc=STOP;
        c.jit_chain_budget=256;
        c.pc=region(&c,e.get());
        if(c.pc!=A+4096 || c.regs[1]!=2000) return false;
        while(c.pc!=STOP) c.pc=j->run_block(c,*e);
        return c.regs[1]==2100;
    }
    return j->lookup_only(A)==region && execute(2100) && execute(2100);
}
int main(int argc,char**argv) {
    const bool mt=argc>1 && std::string(argv[1])=="mt";
    if(!rewritten_exit(mt) || !rewritten_exit(false,true) || !scenario(mt,false) || (mt && !scenario(true,true))) {
        std::fputs("tier2 feedback oracle failed\n",stderr); return 1;
    }
    std::puts("jit_tier2_feedback: neighboring regions, recoveries and concurrent phases PASS");
}
