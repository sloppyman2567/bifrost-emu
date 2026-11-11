#include <algorithm>
// Exercise real CPU-time decisions with controlled slowdowns, never fake timers.
#include "System/Core/Emulator.hpp"
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include <cstdio>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
using namespace arm64emu;
using Fn=uint64_t (*)(CPU*,Emulator*);
static Fn baseline_native,baseline_chain,candidate_native,candidate_chain;
static volatile uint64_t burner=1;
static unsigned candidate_delay=0;
// Burn counts are calibrated so that every timed visit lasts longer than a trial
// window: the scenarios below count one closed window per visit.
static unsigned base_burn=200000;
static unsigned faster() { return base_burn*6/10; }
static bool linear_measure=false;
static std::atomic<bool> armed{false},ready{false},release_trial{false};
static void burn(unsigned count) {
    for(unsigned k=0;k<count;++k) burner=burner*1664525+1013904223;
}
static uint64_t slow_baseline(CPU* c,Emulator* e) {
    if(linear_measure || c->jit_tier2_trial_remaining==256) burn(base_burn);
    return baseline_native(c,e);
}
static uint64_t slow_baseline_chain(CPU* c,Emulator* e) {
    if(linear_measure || c->jit_tier2_trial_remaining==256) burn(base_burn);
    return baseline_chain(c,e);
}
static uint64_t slow_candidate(CPU* c,Emulator* e) {
    if(linear_measure || c->jit_tier2_trial_remaining==256) {
        if(armed.load(std::memory_order_acquire)) {
            ready.store(true,std::memory_order_release);
            while(!release_trial.load(std::memory_order_acquire)) std::this_thread::yield();
        }
        burn(candidate_delay);
    }
    return candidate_native(c,e);
}
static uint64_t slow_candidate_chain(CPU* c,Emulator* e) {
    if(linear_measure || c->jit_tier2_trial_remaining==256) burn(candidate_delay);
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
        slot.owner=t->monitor->identity; slot.baseline_ns=FrostJIT::kTier2TrialWindowNs; slot.baseline_work=128;
    }
    // A retired block's predecessor must chain to its rebuilt translation.
    static bool relinked(FrostJIT& j,uint64_t target,uint64_t source) {
        const auto refs=j.back_refs_.find(target);
        return refs!=j.back_refs_.end() &&
            std::find(refs->second.begin(),refs->second.end(),source)!=refs->second.end();
    }
    static unsigned pairs() { return FrostJIT::kTier2TrialPairs; }
    static uint64_t window_ns() { return FrostJIT::kTier2TrialWindowNs; }
    static unsigned decision(void* ptr) { return static_cast<FrostJIT::Tier2Trial*>(ptr)->decision.load(); }
    static bool linear_window(CPU& c,void* ptr) {
        auto* t=static_cast<FrostJIT::Tier2Trial*>(ptr);
        auto& slot=c.jit_tier2_feedback[t->monitor->slot];
        slot.owner=t->monitor->identity;
        slot.sample_quota=(128+t->candidate_entry.instr_count-1)/t->candidate_entry.instr_count;
        return t->linear;
    }
    static unsigned block_count(void* ptr) {
        return static_cast<FrostJIT::Tier2Trial*>(ptr)->baseline_blocks.size();
    }
    static unsigned limit(FrostJIT& j) {
        auto it=j.tier2_block_limits_.find(0x10000);
        return it==j.tier2_block_limits_.end()?0:it->second;
    }
    static size_t trials(FrostJIT& j) { return j.tier2_trials_.size(); }
    static void exhaust_budget(FrostJIT& j) { j.tier2_code_bytes.store(32ull*1024*1024); }
    static bool profiles_reset(FrostJIT& j,unsigned blocks) {
        for(unsigned k=0;k<blocks;++k)
            if(j.tier2_profiles_by_pc_.count(0x10000+k*8+4)) return false;
        return true;
    }
    static void change_phase(FrostJIT& j) {
        auto* monitor=j.blocks_.at(0x10000).tier2_monitor;
        CPU c;
        // Exits with no completed iteration: every feedback window is bad, and the last strike retires.
        for(unsigned k=0;k<FrostJIT::kTier2FeedbackWindowExits*FrostJIT::kTier2FeedbackStrikes;++k)
            j.tier2_region_feedback(c,monitor);
    }
};
}
constexpr uint64_t HEAD=0x10000,STOP=0x20000;
static bool scenario(bool mt,bool keep,bool uneven=false) {
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
    candidate_delay=keep?faster():base_burn*10;
    const size_t prefix=j->code_buf_used();
    std::vector<uint8_t> bytes(j->code_buf(),j->code_buf()+prefix);
    CPU c; c.regs[30]=STOP; c.jit_stop_pc=STOP;
    // Uneven visits leave the loop long before the quota, as real loops do:
    // they still have to reach a verdict, by time per guest instruction.
    const unsigned runs=(uneven?16:2)*Tier2TestAccess::pairs();
    for(unsigned k=0;k<runs && !Tier2TestAccess::decision(trial);++k) {
        c.pc=HEAD; c.regs[0]=uneven?20+k%7:300; c.regs[1]=0;
        const auto want=c.regs[0];
        auto next=j->tier2_measure(c,*e,trial);
        if(uneven) {
            if(c.regs[1]!=want || c.regs[0] || c.jit_tier2_trial) return false;
            continue;
        }
        if(c.regs[1]!=256 || c.regs[0]!=want-256 || next!=HEAD || c.jit_tier2_trial) {
            std::fprintf(stderr,"bad trial execution pc=%llx n=%llu sum=%llu\n",(unsigned long long)next,
                (unsigned long long)c.regs[0],(unsigned long long)c.regs[1]); return false;
        }
    }
    const unsigned expected=keep?1:2;
    if(Tier2TestAccess::decision(trial)!=expected ||
        j->tier2_measured_pairs.load()!=Tier2TestAccess::pairs() ||
        j->tier2_measured_keeps.load()!=(expected==1?1:0) ||
        j->tier2_measured_rejections.load()!=(expected==2?1:0)) {
        std::fprintf(stderr,"wrong measured decision: got=%u pairs=%llu keeps=%llu rejects=%llu\n",
            Tier2TestAccess::decision(trial),(unsigned long long)j->tier2_measured_pairs.load(),
            (unsigned long long)j->tier2_measured_keeps.load(),(unsigned long long)j->tier2_measured_rejections.load());
        return false;
    }
    if(mt && std::memcmp(bytes.data(),j->code_buf(),prefix)) return false;
    c.pc=HEAD; c.regs[0]=300; c.regs[1]=0;
    c.jit_tier2_trial_work=123;
    while(c.pc!=STOP) c.pc=j->run_block(c,*e);
    return c.regs[1]==300 && c.regs[0]==0 && c.jit_tier2_trial_work==123;
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
    candidate_delay=faster();
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
    candidate_delay=faster();
    CPU c; c.regs[30]=STOP; c.jit_stop_pc=STOP;
    for(unsigned k=0;k<2*Tier2TestAccess::pairs()-1;++k) {
        c.pc=HEAD; c.regs[0]=300; c.regs[1]=0;
        if(j->tier2_measure(c,*e,trial)!=HEAD || c.regs[1]!=256) return false;
    }
    if(j->tier2_measured_pairs.load()!=Tier2TestAccess::pairs()-1) return false;
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
    if(j->tier2_measured_pairs.load()!=Tier2TestAccess::pairs()-1 || j->tier2_measured_keeps.load() ||
        j->lookup_only(HEAD) || c.jit_tier2_trial || c.regs[1]+c.regs[0]!=300 ||
        std::memcmp(bytes.data(),j->code_buf(),prefix)) return false;
    CPU fresh; fresh.pc=HEAD; fresh.regs[0]=2; fresh.regs[30]=STOP; fresh.jit_stop_pc=STOP;
    if(stale(&fresh,e.get())!=HEAD || fresh.regs[1]) return false;
    while(fresh.pc!=STOP) fresh.pc=j->run_block(fresh,*e);
    return fresh.regs[1]==200;
}
static bool linear_scenario(bool mt,bool keep,bool collisions=false) {
    linear_measure=true;
    auto e=std::make_unique<Emulator>();
    e->mem().map_range(HEAD,4096); e->mem().mprotect_guest(HEAD,4096,7);
    std::vector<uint32_t> code;
    for (unsigned k=0;k<8;++k) {
        code.push_back(0x91000421); // add x1,x1,#1
        code.push_back(0x14000001); // b next block
    }
    code.push_back(0xd65f03c0);
    e->mem().write(HEAD,code.data(),code.size()*4); e->enable_jit();
    auto* j=e->jit(); if(mt) j->enter_multithreaded();
    for(unsigned k=0;k<40;++k) {
        CPU warm; warm.pc=HEAD; warm.regs[30]=STOP; warm.jit_stop_pc=STOP;
        while(warm.pc!=STOP) warm.pc=j->run_block(warm,*e);
        if(warm.regs[1]!=8) return false;
    }
    void* trial=Tier2TestAccess::prepare(*j,HEAD);
    CPU c;
    if(!trial) return false;
    candidate_delay=keep ? faster() : base_burn*2;
    const size_t prefix=j->code_buf_used();
    std::vector<uint8_t> bytes(j->code_buf(),j->code_buf()+prefix);
    unsigned visits=0;
    // Colliding visits never close a window (their work stays under the
    // minimum), so the visit budget alone ends the trial: burn little.
    struct BurnScope { unsigned saved=base_burn; ~BurnScope() { base_burn=saved; } } burn_scope;
    if(collisions) base_burn=1000;
    for(;visits<262144 && !Tier2TestAccess::decision(trial);++visits) {
        c.pc=HEAD; c.regs[1]=0; c.regs[30]=STOP; c.jit_stop_pc=STOP;
        // A colliding owner between every visit prevents a pair from
        // forming; the global attempt budget must still terminate.
        if(collisions) for(auto& slot:c.jit_tier2_feedback) slot.owner=0;
        if(j->tier2_measure(c,*e,trial)!=HEAD+64 || c.regs[1]!=8 || c.jit_tier2_trial)
            return false;
    }
    const unsigned expected=keep&&!collisions ? 1 : 2;
    if(Tier2TestAccess::decision(trial)!=expected ||
        j->tier2_measured_pairs.load()!=(collisions ? 0 : Tier2TestAccess::pairs()) ||
        (!collisions&&visits>16*Tier2TestAccess::pairs()) ||
        (mt && std::memcmp(bytes.data(),j->code_buf(),prefix))) {
        std::fprintf(stderr,"linear timing decision mismatch: decision=%u expected=%u pairs=%llu\n",
            Tier2TestAccess::decision(trial),expected,(unsigned long long)j->tier2_measured_pairs.load());
        return false;
    }
    if(!keep && !collisions && j->lookup_only(HEAD)) {
        std::fputs("losing eight-block trial did not rearm a shorter candidate\n",stderr);
        return false;
    }
    c.pc=HEAD; c.regs[1]=0;
    c.jit_tier2_trial_work=123;
    while(c.pc!=STOP) c.pc=j->run_block(c,*e);
    linear_measure=false;
    return c.regs[1]==8 && c.jit_tier2_trial_work==123;
}

// Real timing rejects 8 -> 4 -> 2, or selects the shorter winner. Retired
// wrappers must become inert; new translations need another hotness window.
static bool shorter_candidates(bool mt,bool keep_four,bool exhaust=false,bool invalidate=false) {
    linear_measure=true;
    auto e=std::make_unique<Emulator>();
    e->mem().map_range(HEAD,4096); e->mem().mprotect_guest(HEAD,4096,7);
    std::vector<uint32_t> code;
    for(unsigned k=0;k<8;++k) {
        code.push_back(0x91000421);
        code.push_back(0xb5000022); // cbnz x2,next (both paths continue; profiles still warm)
    }
    code.push_back(0xd65f03c0);
    // A predecessor outside every region: nothing retranslates it, so only
    // the rebuild after retirement can chain it to the head again.
    const uint64_t outside=HEAD+code.size()*4;
    code.push_back(0x14000000|((0u-static_cast<uint32_t>(code.size()))&0x3ffffff)); // b HEAD
    e->mem().write(HEAD,code.data(),code.size()*4); e->enable_jit();
    auto* j=e->jit(); if(mt) j->enter_multithreaded();
    auto visit_from=[&](uint64_t pc) {
        CPU c; c.pc=pc; c.regs[30]=STOP; c.jit_stop_pc=STOP;
        for(unsigned n=0;c.pc!=STOP && n<1000;++n) c.pc=j->run_block(c,*e);
        return c.pc==STOP && c.regs[1]==8;
    };
    auto visit=[&]() { return visit_from(HEAD); };
    if(!visit_from(outside) || !visit_from(outside)) return false;
    if(!mt && !Tier2TestAccess::relinked(*j,HEAD,outside)) {
        std::fputs("outside predecessor never chained to the head\n",stderr);
        return false;
    }
    for(unsigned size=8;size>=2;size/=2) {
        for(unsigned k=0;k<40;++k) if(!visit()) return false;
        void* trial=Tier2TestAccess::prepare(*j,HEAD);
        if(!trial || Tier2TestAccess::block_count(trial)!=size) {
            std::fprintf(stderr,"missing %u-block retry\n",size); return false;
        }
        if(exhaust) Tier2TestAccess::exhaust_budget(*j);
        const bool keep=keep_four && size==4;
        candidate_delay=keep?faster():base_burn*2;
        const size_t prefix=j->code_buf_used();
        std::vector<uint8_t> bytes(j->code_buf(),j->code_buf()+prefix);
        CPU c; Tier2TestAccess::linear_window(c,trial);
        for(unsigned k=0;k<2048 && !Tier2TestAccess::decision(trial);++k) {
            c.pc=HEAD; c.regs[1]=0; c.regs[30]=STOP; c.jit_stop_pc=STOP;
            if(j->tier2_measure(c,*e,trial)!=HEAD+size*8 || c.regs[1]!=size) return false;
        }
        if(Tier2TestAccess::decision(trial)!=(keep?1u:2u) ||
            (mt && std::memcmp(bytes.data(),j->code_buf(),prefix))) return false;
        if(keep || size==2 || exhaust) {
            const auto trials=Tier2TestAccess::trials(*j);
            for(unsigned k=0;k<80;++k) if(!visit()) return false;
            linear_measure=false;
            if(!j->lookup_only(HEAD) || Tier2TestAccess::trials(*j)!=trials ||
                (exhaust && Tier2TestAccess::limit(*j)!=0)) return false;
            if(keep) {
                Tier2TestAccess::change_phase(*j);
                if(j->lookup_only(HEAD) || Tier2TestAccess::limit(*j)) return false;
                for(unsigned k=0;k<40;++k) if(!visit()) return false;
                void* fresh=Tier2TestAccess::prepare(*j,HEAD);
                if(!fresh || Tier2TestAccess::block_count(fresh)!=8) return false;
            }
            return true;
        }
        if(j->lookup_only(HEAD) || Tier2TestAccess::limit(*j)!=size/2 ||
            !Tier2TestAccess::profiles_reset(*j,size)) return false;
        c.pc=HEAD; c.regs[1]=0;
        if(j->tier2_measure(c,*e,trial)!=HEAD || c.regs[1]) return false;
        if(invalidate) {
            // Even between retirement and retranslation, an address reuse
            // must discard the old size preference.
            j->invalidate_range(HEAD,4);
            linear_measure=false;
            return Tier2TestAccess::limit(*j)==0;
        }
        // The next visit translates fresh counters, without immediate retrial.
        const auto trials=Tier2TestAccess::trials(*j);
        if(!visit() || Tier2TestAccess::trials(*j)!=trials) return false;
        if(!mt && !Tier2TestAccess::relinked(*j,HEAD,outside)) {
            std::fputs("retired block lost its predecessor chain\n",stderr);
            return false;
        }
    }
    return false;
}
int main(int argc,char**argv) {
    const bool mt=argc>1 && std::string(argv[1])=="mt";
    {
        const auto t0=std::chrono::steady_clock::now();
        burn(2000000);
        const double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-t0).count();
        const double per_ns=2000000/(ns>1?ns:1);
        base_burn=std::max(200000u,unsigned(2.5*per_ns*Tier2TestAccess::window_ns()));
    }
    if(!cold_and_transition(mt)||!scenario(mt,true)||!scenario(mt,false)||!scenario(mt,true,true)||(mt&&!invalidated_trial())) return 1;
    if(!linear_scenario(mt,true)||!linear_scenario(mt,false)||!linear_scenario(mt,true,true)) return 1;
    if(!shorter_candidates(mt,false)||!shorter_candidates(mt,true)||
       !shorter_candidates(mt,false,true)||!shorter_candidates(mt,false,false,true)) return 1;
    std::puts("jit_tier2_measure: real timing keeps faster code, rejects slower code, decides uneven visits PASS");
}
