// Exercise real CPU-time decisions with controlled slowdowns, never fake timers.
#include "System/Core/Emulator.hpp"
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include <cstdio>
#include <atomic>
#include <thread>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
using namespace arm64emu;
using Fn=uint64_t (*)(CPU*,Emulator*);
static Fn baseline_native,baseline_chain,candidate_native,candidate_chain;
static volatile uint64_t burner=1;
static unsigned candidate_delay=0;
static std::atomic<bool> armed{false},ready{false},release_trial{false};
static void burn(unsigned count) {
    for(unsigned k=0;k<count;++k) burner=burner*1664525+1013904223;
}
static uint64_t slow_baseline(CPU* c,Emulator* e) {
    if(c->jit_tier2_trial_remaining==256) burn(200000);
    return baseline_native(c,e);
}
static uint64_t slow_baseline_chain(CPU* c,Emulator* e) {
    if(c->jit_tier2_trial_remaining==256) burn(200000);
    return baseline_chain(c,e);
}
static uint64_t slow_candidate(CPU* c,Emulator* e) {
    if(c->jit_tier2_trial_remaining==256) {
        if(armed.load(std::memory_order_acquire)) {
            ready.store(true,std::memory_order_release);
            while(!release_trial.load(std::memory_order_acquire)) std::this_thread::yield();
        }
        burn(candidate_delay);
    }
    return candidate_native(c,e);
}
static uint64_t slow_candidate_chain(CPU* c,Emulator* e) {
    if(c->jit_tier2_trial_remaining==256) burn(candidate_delay);
    return candidate_chain(c,e);
}
namespace arm64emu {
struct Tier2TestAccess {
    static void* prepare(FrostJIT& j,uint64_t head) {
        auto it=j.blocks_.find(head);
        if(it==j.blocks_.end() || !it->second.tier2_monitor) return nullptr;
        auto* t=it->second.tier2_monitor->trial;
        if(!t || t->decision.load() || !t->baseline_sample || !t->candidate_sample ||
           t->baseline_sample==t->baseline.fn || t->candidate_sample==t->candidate) return nullptr;
        t->attempts=0; t->ratios.clear();
        baseline_native=t->baseline_sample; baseline_chain=t->baseline_chain_sample;
        candidate_native=t->candidate_sample; candidate_chain=t->candidate_chain_sample;
        t->baseline_sample=slow_baseline;
        if(baseline_chain) t->baseline_chain_sample=slow_baseline_chain;
        t->candidate_sample=slow_candidate;
        if(candidate_chain) t->candidate_chain_sample=slow_candidate_chain;
        return t;
    }
    static void candidate_window(CPU& c,void* ptr) {
        auto* t=static_cast<FrostJIT::Tier2Trial*>(ptr);
        auto& slot=c.jit_tier2_feedback[t->monitor->slot];
        slot.owner=t->monitor->identity; slot.baseline_ns=10000;
    }
    static void mismatch(CPU& c,void* ptr) {
        auto* t=static_cast<FrostJIT::Tier2Trial*>(ptr);
        ++c.jit_tier2_feedback[t->monitor->slot].baseline_work;
    }
    static unsigned decision(void* ptr) { return static_cast<FrostJIT::Tier2Trial*>(ptr)->decision.load(); }
};
}
constexpr uint64_t HEAD=0x10000,STOP=0x20000;
static bool scenario(bool mt,bool keep,bool mismatch=false) {
    auto e=std::make_unique<Emulator>();
    e->mem().map_range(HEAD,4096); e->mem().mprotect_guest(HEAD,4096,7);
    const uint32_t code[]={0x91000421,0x14000001,0xd1000400,0xb5ffffa0,0xd65f03c0};
    e->mem().write(HEAD,code,sizeof(code)); e->enable_jit();
    auto* j=e->jit(); if(mt) j->enter_multithreaded();
    CPU warm; warm.pc=HEAD; warm.regs[0]=40; warm.regs[30]=STOP; warm.jit_stop_pc=STOP;
    for(unsigned n=0;warm.pc!=STOP && n<10000;++n) warm.pc=j->run_block(warm,*e);
    if(warm.pc!=STOP || warm.regs[1]!=40) return false;
    void* trial=Tier2TestAccess::prepare(*j,HEAD);
    if(!trial) { std::fputs("missing undecided trial\n",stderr); return false; }
    candidate_delay=keep?20000:2000000;
    const size_t prefix=j->code_buf_used();
    std::vector<uint8_t> bytes(j->code_buf(),j->code_buf()+prefix);
    CPU c; c.regs[30]=STOP; c.jit_stop_pc=STOP;
    const unsigned runs=mismatch?128:16;
    for(unsigned k=0;k<runs;++k) {
        c.pc=HEAD; c.regs[0]=300; c.regs[1]=0;
        const auto want=c.regs[0];
        auto next=j->tier2_measure(c,*e,trial);
        if(c.regs[1]!=256 || c.regs[0]!=want-256 || next!=HEAD || c.jit_tier2_trial) {
            std::fprintf(stderr,"bad trial execution pc=%llx n=%llu sum=%llu\n",(unsigned long long)next,
                (unsigned long long)c.regs[0],(unsigned long long)c.regs[1]); return false;
        }
        if(mismatch && k%2==0) Tier2TestAccess::mismatch(c,trial);
    }
    const unsigned expected=keep&&!mismatch?1:2;
    if(Tier2TestAccess::decision(trial)!=expected ||
        j->tier2_measured_pairs.load()!=(mismatch?0:8) ||
        j->tier2_measured_keeps.load()!=(expected==1?1:0) ||
        j->tier2_measured_rejections.load()!=(expected==2?1:0)) {
        std::fprintf(stderr,"wrong measured decision: got=%u pairs=%llu keeps=%llu rejects=%llu\n",
            Tier2TestAccess::decision(trial),(unsigned long long)j->tier2_measured_pairs.load(),
            (unsigned long long)j->tier2_measured_keeps.load(),(unsigned long long)j->tier2_measured_rejections.load());
        return false;
    }
    if(mt && std::memcmp(bytes.data(),j->code_buf(),prefix)) return false;
    c.pc=HEAD; c.regs[0]=300; c.regs[1]=0;
    c.jit_tier2_trial_work=123; c.jit_tier2_trial_hash=456;
    while(c.pc!=STOP) c.pc=j->run_block(c,*e);
    return c.regs[1]==300 && c.regs[0]==0 &&
        c.jit_tier2_trial_work==123 && c.jit_tier2_trial_hash==456;
}
// A cold exit must stop before its RET, even when that target is cached.
// Transition also retires the private ST graph before another CPU can run.
static bool cold_and_transition(bool mt) {
    auto e=std::make_unique<Emulator>();
    e->mem().map_range(HEAD,4096); e->mem().mprotect_guest(HEAD,4096,7);
    const uint32_t code[]={0x91000421,0x14000001,0xd1000400,0xb5ffffa0,0xd65f03c0};
    e->mem().write(HEAD,code,sizeof(code)); e->enable_jit();
    auto* j=e->jit(); if(mt) j->enter_multithreaded();
    CPU warm; warm.pc=HEAD; warm.regs[0]=40; warm.regs[30]=STOP; warm.jit_stop_pc=STOP;
    while(warm.pc!=STOP) warm.pc=j->run_block(warm,*e);
    void* trial=Tier2TestAccess::prepare(*j,HEAD); if(!trial) return false;
    candidate_delay=20000;
    for(unsigned arm=0;arm<2;++arm) {
        CPU c; c.pc=HEAD; c.regs[0]=1; c.regs[30]=STOP; c.jit_stop_pc=STOP;
        if(arm) Tier2TestAccess::candidate_window(c,trial);
        if(j->tier2_measure(c,*e,trial)!=HEAD+16 || c.regs[0] || c.regs[1]!=1 || c.jit_tier2_trial)
            return false;
    }
    if(j->tier2_measured_pairs.load() || j->tier2_measured_keeps.load()) return false;
    if(!mt) {
        const auto stale=j->lookup_only(HEAD);
        j->enter_multithreaded();
        CPU c; c.pc=HEAD; c.regs[0]=2; c.regs[30]=STOP; c.jit_stop_pc=STOP;
        if(stale(&c,e.get())!=HEAD || c.regs[1]) return false;
        while(c.pc!=STOP) c.pc=j->run_block(c,*e);
        if(c.regs[1]!=2) return false;
    }
    return true;
}
static bool invalidated_trial() {
    auto e=std::make_unique<Emulator>();
    e->mem().map_range(HEAD,4096); e->mem().mprotect_guest(HEAD,4096,7);
    const uint32_t code[]={0x91000421,0x14000001,0xd1000400,0xb5ffffa0,0xd65f03c0};
    e->mem().write(HEAD,code,sizeof(code)); e->enable_jit();
    auto* j=e->jit(); j->enter_multithreaded();
    CPU warm; warm.pc=HEAD; warm.regs[0]=40; warm.regs[30]=STOP; warm.jit_stop_pc=STOP;
    while(warm.pc!=STOP) warm.pc=j->run_block(warm,*e);
    void* trial=Tier2TestAccess::prepare(*j,HEAD); if(!trial) return false;
    candidate_delay=20000;
    CPU c; c.regs[30]=STOP; c.jit_stop_pc=STOP;
    for(unsigned k=0;k<15;++k) {
        c.pc=HEAD; c.regs[0]=300; c.regs[1]=0;
        if(j->tier2_measure(c,*e,trial)!=HEAD || c.regs[1]!=256) return false;
    }
    if(j->tier2_measured_pairs.load()!=7) return false;
    const auto stale=j->lookup_only(HEAD);
    const size_t prefix=j->code_buf_used();
    std::vector<uint8_t> bytes(j->code_buf(),j->code_buf()+prefix);
    ready.store(false); release_trial.store(false); armed.store(true);
    c.pc=HEAD; c.regs[0]=300; c.regs[1]=0;
    std::thread runner([&] { j->tier2_measure(c,*e,trial); });
    while(!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    const uint32_t changed=0x91019021; // add x1,x1,#100
    e->mem().write(HEAD,&changed,4); j->invalidate_range(HEAD,4);
    release_trial.store(true,std::memory_order_release); runner.join(); armed.store(false);
    if(j->tier2_measured_pairs.load()!=7 || j->tier2_measured_keeps.load() ||
        j->lookup_only(HEAD) || c.jit_tier2_trial || c.regs[1]+c.regs[0]!=300 ||
        std::memcmp(bytes.data(),j->code_buf(),prefix)) return false;
    CPU fresh; fresh.pc=HEAD; fresh.regs[0]=2; fresh.regs[30]=STOP; fresh.jit_stop_pc=STOP;
    if(stale(&fresh,e.get())!=HEAD || fresh.regs[1]) return false;
    while(fresh.pc!=STOP) fresh.pc=j->run_block(fresh,*e);
    return fresh.regs[1]==200;
}
int main(int argc,char**argv) {
    const bool mt=argc>1 && std::string(argv[1])=="mt";
    if(!cold_and_transition(mt)||!scenario(mt,true)||!scenario(mt,false)||!scenario(mt,true,true)||(mt&&!invalidated_trial())) return 1;
    std::puts("jit_tier2_measure: real timing keeps faster code, rejects slower/mismatched samples PASS");
}
