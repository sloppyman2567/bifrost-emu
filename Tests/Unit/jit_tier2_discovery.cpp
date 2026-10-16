// Region discovery must follow direct jumps and instruction-cap boundaries,
// preserve loop-carried state at early exits, and invalidate non-head pages.
#include "System/Core/Emulator.hpp"
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "tier2_test_access.hpp"
#include <cstdio>
#include <memory>
#include <vector>
#include <string>
#include <chrono>
#include <cstring>
#include <cstdlib>
using namespace arm64emu;
namespace arm64emu { extern "C" uint64_t jit_call_helper(CPU*, Emulator*, uint64_t); }

static bool check_region_invariant(bool mt) {
    constexpr uint64_t head=0x60000;
    auto e=std::make_unique<Emulator>();
    e->mem().map_range(head,4096); e->mem().mprotect_guest(head,4096,7);
    // Twelve invariants exceed the available keep registers. Each one's last
    // body use is a destructive-add candidate, but its spill home is live on
    // the next trip. This reproduces zstd's overwritten preheader values.
    std::vector<uint32_t> code;
    for (unsigned a=1;a<=12;++a) {
        code.push_back(0x91000000u | (a<<10) | (15<<5) | 16); // add x16,x15,#a
        code.push_back(0x8b000000u | (a<<16) | (16<<5) | a); // add xa,x16,xa
    }
    code.push_back(0xd1000400);
    code.push_back(0xb5000000u | ((uint32_t(-int(code.size()))&0x7ffff)<<5));
    e->mem().write(head,code.data(),code.size()*4); e->enable_jit();
    auto* j=e->jit(); if (mt) j->enter_multithreaded();
    const auto trace=j->collect_tier2_trace(*e,head);
    auto fn=j->compile_tier2_region(*e,trace);
    if (!fn) return false;
    for (uint64_t count : {1ULL,2ULL,7ULL,257ULL}) {
        CPU c; c.pc=head; c.regs[0]=count; c.regs[15]=5;
        unsigned entries=0;
        while (c.pc==head && ++entries<=count+1) {
            c.jit_chain_budget=256; c.pc=fn(&c,e.get());
        }
        if (c.pc!=head+code.size()*4 || c.regs[15]!=5) return false;
        for (unsigned a=1;a<=12;++a)
            if (c.regs[a]!=count*(5+a)) {
                std::fprintf(stderr,"region invariant overwritten: n=%llu x%u=%llu want=%llu\n",
                    (unsigned long long)count,a,(unsigned long long)c.regs[a],
                    (unsigned long long)(count*(5+a))); return false;
            }
    }
    if (!Tier2TestAccess::region_invariants_preserved(*j,trace)) {
        std::fputs("region body redefines a preheader home\n",stderr); return false;
    }
    return true;
}

// The first exit precedes all vector writes. After a backedge it must commit
// values carried from the previous trip, even though that exit is statically
// clean. The scalar-FP variant shares the cache but keeps v_hi in memory.
static bool check_region_vectors(bool mt, bool benchmark) {
    constexpr uint64_t head=0x10000, body=0x11000, exit=0x12000, data=0x30000;
    for (uint32_t arithmetic : {0x4e61d400u, 0x0e21d400u, 0x1e612800u, 0x1e212800u}) {
        auto e=std::make_unique<Emulator>();
        e->mem().map_range(head,0x3000);
        e->mem().mprotect_guest(head,0x3000,7);
        const uint32_t entry[]={0xb4010000,0x140003ff}; // cbz x0,exit; b body
        const uint32_t loop[]={arithmetic,0xd1000400,0x17fffbfe}; // sub x0,#1; b head
        e->mem().write(head,entry,sizeof(entry));
        e->mem().write(body,loop,sizeof(loop));
        e->enable_jit();
        auto* j=e->jit();
        if (mt) j->enter_multithreaded();
        auto trace=j->collect_tier2_trace(*e,head);
        auto fn=j->compile_tier2_region(*e,trace);
        const bool scalar=arithmetic==0x1e612800u || arithmetic==0x1e212800u;
        const bool expect_cache=Tier2TestAccess::supports_vector_cache(*j) &&
            !std::getenv(scalar ? "BIFROST_NO_FP_CACHE" : "BIFROST_NO_VEC_CACHE");
        if (!fn || Tier2TestAccess::vector_cache_active(*j)!=expect_cache) {
            std::fprintf(stderr,"region vector cache inactive: op=%08x\n",arithmetic);
            return false;
        }
        auto init=[&](CPU& c,uint64_t count,bool nan) {
            c.pc=head; c.regs[0]=count;
            if (arithmetic==0x4e61d400u || arithmetic==0x1e612800u) {
                const double one=1.0,two=2.0;
                std::memcpy(&c.v_lo[1],&one,8); std::memcpy(&c.v_hi[1],&two,8);
                if (nan) c.v_lo[1]=UINT64_C(0x7ff8000000001234);
            } else {
                c.v_lo[1]=UINT64_C(0x400000003f800000);
                c.v_hi[1]=UINT64_C(0x4080000040400000);
                if (nan) c.v_lo[1]=UINT64_C(0x400000007fc01234);
            }
            c.v_hi[0]=UINT64_C(0x3ff000003f800000);
        };
        for (uint64_t count : {0ULL,1ULL,7ULL,257ULL}) for (bool nan : {false,true}) {
            CPU actual,reference; init(actual,count,nan); init(reference,count,nan);
            unsigned entries=0;
            FrostJIT::FaultScope scope(j);
            while (actual.pc==head && ++entries<=count+2) {
                actual.jit_chain_budget=256;
                actual.pc=fn(&actual,e.get());
            }
            while (reference.pc!=exit) e->step(reference);
            if (actual.pc!=exit || actual.regs[0]!=reference.regs[0] ||
                actual.v_lo[0]!=reference.v_lo[0] || actual.v_hi[0]!=reference.v_hi[0]) {
                std::fprintf(stderr,"region vector mismatch: op=%08x n=%llu nan=%d lo=%llx/%llx hi=%llx/%llx\n",
                    arithmetic,(unsigned long long)count,nan,
                    (unsigned long long)actual.v_lo[0],(unsigned long long)reference.v_lo[0],
                    (unsigned long long)actual.v_hi[0],(unsigned long long)reference.v_hi[0]);
                return false;
            }
        }
        if (benchmark) {
            CPU c; init(c,2000000,false);
            c.v_hi[0]=0;
            const auto begin=std::chrono::steady_clock::now();
            FrostJIT::FaultScope scope(j);
            while (c.pc==head) { c.jit_chain_budget=256; c.pc=fn(&c,e.get()); }
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            uint64_t lo=0,hi=0;
            if (arithmetic==0x4e61d400u || arithmetic==0x1e612800u) {
                const double low=2000000.0,high=4000000.0;
                std::memcpy(&lo,&low,8);
                if (!scalar) std::memcpy(&hi,&high,8);
            } else {
                const float low=2000000.0f,high=4000000.0f;
                uint32_t low_bits,high_bits;
                std::memcpy(&low_bits,&low,4); std::memcpy(&high_bits,&high,4);
                lo=low_bits;
                if (!scalar) lo|=uint64_t(high_bits)<<32;
            }
            if (c.pc!=exit || c.regs[0] || c.v_lo[0]!=lo || c.v_hi[0]!=hi) {
                std::fputs("region vector benchmark oracle failed\n",stderr); return false;
            }
            std::printf("region vector op=%08x: %.3f ms\n",arithmetic,ms);
        }
        // Fault at the head on the second trip, before any new FP write.
        // Recovery must publish the carried XMM state before stepping LDR.
        j->invalidate_all();
        e->mem().map_range(data,4096);
        const uint32_t fault_head[]={0xf9400085,0xb400ffe0,0x140003fe};
        const uint32_t fault_body[]={arithmetic,0x91400484,0xd1000400,0x17fffbfd};
        e->mem().write(head,fault_head,sizeof(fault_head));
        e->mem().write(body,fault_body,sizeof(fault_body));
        trace=j->collect_tier2_trace(*e,head);
        fn=j->compile_tier2_region(*e,trace);
        if (!fn) return false;
        CPU c,reference; init(c,2,false); init(reference,1,false);
        c.regs[4]=data; reference.regs[4]=data;
        while (reference.pc!=head || reference.regs[0]) e->step(reference);
        {
            FrostJIT::FaultScope scope(j);
            unsigned entries=0;
            while (c.running && c.pc==head && ++entries<4) {
                c.jit_chain_budget=256; c.pc=fn(&c,e.get());
            }
        }
        if (c.running || c.exit_code!=139 || c.v_lo[0]!=reference.v_lo[0] ||
            c.v_hi[0]!=reference.v_hi[0]) {
            std::fprintf(stderr,"region vector fault-state mismatch: op=%08x\n",arithmetic);
            return false;
        }
    }
    return true;
}

// Inline widening/conversions must retain the region layout across a cold
// NaN step and an exit preceding the next trip's first vector write.
static bool check_region_native_simd(bool mt) {
    constexpr uint64_t head=0x40000, body=0x41000, exit=0x42000;
    for (bool half : {false,true}) {
        auto e=std::make_unique<Emulator>();
        e->mem().map_range(head,0x3000); e->mem().mprotect_guest(head,0x3000,7);
        e->enable_jit(); auto* j=e->jit();
        if (half && !j->cpu_features().has_f16c()) continue;
        if (mt) j->enter_multithreaded();
        const uint32_t entry[]={0xb4010000,0x140003ff};
        const uint32_t loop[]={
            half ? 0x0e217862u : 0x2f10a462u, // fcvtl v2.4s,v3.4h / ushll v2.4s,v3.4h,#0
            0x6e21d844,                      // ucvtf v4.4s,v2.4s
            0x4e21d400,                      // fadd v0.4s,v0.4s,v1.4s
            0x4f108484,                      // shrn2 v4.8h,v4.4s,#16 (aliased source)
            0xd1000400,0x17fffbfb};           // sub x0,#1; b head
        e->mem().write(head,entry,sizeof(entry)); e->mem().write(body,loop,sizeof(loop));
        auto trace=j->collect_tier2_trace(*e,head);
        auto fn=j->compile_tier2_region(*e,trace);
        const bool expect_cache=Tier2TestAccess::supports_vector_cache(*j) &&
            !std::getenv("BIFROST_NO_VEC_CACHE");
        if (!fn || Tier2TestAccess::vector_cache_active(*j)!=expect_cache) return false;
        for (unsigned n : {0u,1u,7u,257u}) for (bool nan : {false,true}) {
            CPU actual,reference;
            actual.pc=head; actual.regs[0]=n;
            actual.v_lo[1]=UINT64_C(0x400000003f800000);
            actual.v_hi[1]=UINT64_C(0x4080000040400000);
            actual.v_lo[3]=nan ? UINT64_C(0x4400420040007d55) : UINT64_C(0x4400420040003c00);
            actual.fpcr=1u<<25;
            reference.pc=actual.pc; reference.regs[0]=n; reference.fpcr=actual.fpcr;
            std::memcpy(reference.v_lo,actual.v_lo,sizeof(actual.v_lo));
            std::memcpy(reference.v_hi,actual.v_hi,sizeof(actual.v_hi));
            unsigned entries=0;
            FrostJIT::FaultScope scope(j);
            while (actual.pc==head && ++entries<=n+2) {
                actual.jit_chain_budget=256; actual.pc=fn(&actual,e.get());
            }
            while (reference.pc!=exit) e->step(reference);
            if (actual.pc!=exit || actual.regs[0]!=reference.regs[0] ||
                std::memcmp(actual.v_lo,reference.v_lo,sizeof(actual.v_lo)) ||
                std::memcmp(actual.v_hi,reference.v_hi,sizeof(actual.v_hi))) {
                std::fprintf(stderr,"region inline SIMD mismatch: half=%d n=%u nan=%d\n",half,n,nan);
                return false;
            }
        }
    }
    return true;
}

// Tier-1 pressure must keep useful vectors resident even across an op
// borrowing XMM3-15. A deferred scalar fault guard precedes later XMM
// reuse, so recovery must use the layout captured at that exact access.
static bool check_tier1_vector_allocation(bool mt) {
    constexpr uint64_t head=0x70000, data=0x80000;
    const uint32_t boundaries[]={
        0xd503201f, // nop
        0x4e218a3e, // frintn v30.4s,v17.4s (C++ helper)
        0x4e205a3e, // cnt v30.16b,v17.16b (borrows high scratch XMMs)
        0x4e3f463e, // sshl v30.4s,v17.4s,v31.4s (XMM3-5 or C++ helper)
        0x4ea1da3e, // frecpe v30.4s,v17.4s (interpreter boundary)
        0x1e3f2a3e, // fadd s30,s17,s31 (mixed scalar/vector aliases)
    };
    for (unsigned accumulators : {14u,28u}) for (uint32_t boundary : boundaries) {
        auto e=std::make_unique<Emulator>();
        e->mem().map_range(head,4096); e->mem().mprotect_guest(head,4096,7);
        e->mem().map_range(data,4096);
        std::vector<uint32_t> code;
        for (unsigned pass=0;pass<(accumulators==14?2u:1u);++pass)
        for (unsigned v=0;v<accumulators;++v) {
            code.push_back(0x4e3fd400u | (v<<5) | v); // fadd vv.4s,vv.4s,v31.4s
            if (!pass && v==7) code.push_back(0xf9400045); // ldr x5,[x2]
            if (!pass && v==11) code.push_back(boundary);
        }
        code.push_back(0xd1000400);
        code.push_back(0xb5000000u | ((uint32_t(-int(code.size()))&0x7ffff)<<5));
        e->mem().write(head,code.data(),code.size()*4);
        e->enable_jit(); auto* j=e->jit();
        if (mt) j->enter_multithreaded();
        auto fn=j->lookup_or_translate(*e,head);
        const bool cache=Tier2TestAccess::supports_vector_cache(*j) &&
                         !std::getenv("BIFROST_NO_VEC_CACHE");
        if (!fn || Tier2TestAccess::vector_cache_active(*j)!=cache ||
            (cache && (Tier2TestAccess::fixed_vectors(*j)<0 ||
                       Tier2TestAccess::fixed_vectors(*j)>=13 ||
                       (boundary==0xd503201f && !Tier2TestAccess::vector_evictions(*j))))) {
            std::fprintf(stderr,"Tier-1 vector pressure allocation inactive: op=%08x\n",boundary);
            return false;
        }
        for (unsigned count : {1u,7u,257u}) for (bool nan : {false,true})
        for (bool fault : {false,true}) {
            if (fault && (count!=1 || nan)) continue;
            CPU actual,reference;
            actual.pc=head; actual.regs[0]=count; actual.regs[2]=data+(fault?4096:0);
            actual.fpcr=1u<<25;
            for (unsigned v=0;v<32;++v) {
                const float value=float(v+1);
                uint32_t bits; std::memcpy(&bits,&value,4);
                actual.v_lo[v]=actual.v_hi[v]=uint64_t(bits)|(uint64_t(bits)<<32);
            }
            if (nan) actual.v_lo[0]=UINT64_C(0x3f8000007fc12345);
            reference.copy_arch_state_from(actual);
            const uint64_t stop=fault ? head+8*4 : head+code.size()*4;
            while (reference.pc!=stop) e->step(reference);
            {
                FrostJIT::FaultScope scope(j);
                unsigned entries=0;
                while (actual.running && actual.pc==head && ++entries<=count+2) {
                    actual.jit_chain_budget=256; actual.pc=fn(&actual,e.get());
                }
            }
            if ((fault ? actual.running || actual.exit_code!=139 : actual.pc!=stop) ||
                actual.regs[0]!=reference.regs[0] ||
                std::memcmp(actual.v_lo,reference.v_lo,sizeof(actual.v_lo)) ||
                std::memcmp(actual.v_hi,reference.v_hi,sizeof(actual.v_hi))) {
                std::fprintf(stderr,"Tier-1 vector mismatch: op=%08x n=%u nan=%d fault=%d\n",
                    boundary,count,nan,fault);
                return false;
            }
        }
    }
    return true;
}

// Integer operations must consume dirty allocated vectors, including high
// XMM registers, without a publish/reload boundary on every instruction.
// Full overwrites must not reload old destinations. Mixing scalar writes
// with vector consumers must still clear the architectural upper lanes, and
// a faulting load must leave its old destination intact.
static bool check_allocation_definitions(bool mt) {
    constexpr uint64_t head=0xa0000, stop=0xa1000;
    for (bool mixed : {false,true}) for (bool fault : {false,true}) {
        auto e=std::make_unique<Emulator>();
        e->mem().map_range(head,4096); e->mem().mprotect_guest(head,4096,7);
        std::vector<uint32_t> code;
        if (fault) {
            for (unsigned v=0;v<14;++v) code.push_back(0x4f02e6a0u|v); // movi vv.16b,#0x55
            code.push_back(0x3dc0005f); // ldr q31,[x2]: unmapped
        } else {
            code={0x4f02e6a0,0x4f05e541,0x4e218402}; // movi 55/aa; add v2,v0,v1
            if (mixed) {
                code.push_back(0x1e222820); // fadd s0,s1,s2 (NaN input, zero upper)
                code.push_back(0x9e6700a3); // fmov d3,x5 (zero upper)
                code.push_back(0x4ea11c64); // orr v4.16b,v3.16b,v1.16b
            }
        }
        code.push_back(0xd65f03c0);
        e->mem().write(head,code.data(),code.size()*4); e->enable_jit();
        auto* j=e->jit(); if (mt) j->enter_multithreaded();
        auto fn=j->lookup_or_translate(*e,head);
        if (!fn) return false;
        if (!fault && !mixed && Tier2TestAccess::vector_cache_active(*j) &&
            Tier2TestAccess::vector_reloads(*j)!=0) {
            std::fputs("write-only vector destinations were preloaded\n",stderr); return false;
        }
        CPU actual,reference; actual.pc=head; actual.regs[30]=stop;
        actual.regs[2]=0xbeef000; actual.regs[5]=UINT64_C(0xfedcba9876543210);
        actual.fpcr=1u<<25;
        for (unsigned v=0;v<32;++v) {
            actual.v_lo[v]=UINT64_C(0x1122334455667788)^v;
            actual.v_hi[v]=UINT64_C(0xdeadbeef98765432)^v;
        }
        reference.copy_arch_state_from(actual);
        const uint64_t reference_stop=fault?head+14*4:stop;
        while (reference.pc!=reference_stop) e->step(reference);
        {
            FrostJIT::FaultScope scope(j);
            actual.pc=fn(&actual,e.get());
        }
        if ((fault?actual.running || actual.exit_code!=139:actual.pc!=stop) ||
            std::memcmp(actual.v_lo,reference.v_lo,sizeof(actual.v_lo)) ||
            std::memcmp(actual.v_hi,reference.v_hi,sizeof(actual.v_hi))) {
            std::fprintf(stderr,"allocation definition mismatch: mixed=%d fault=%d\n",mixed,fault);
            return false;
        }
    }
    return true;
}

// Entry demand values are read, evicted, then defined later. On the next
// trip those early reads must see the carried definition, even though the
// allocator had not reached its first write when it emitted the eviction.
// The region variant also exits before any writes on its final trip.
static bool check_demand_entry_carry(bool mt) {
    constexpr uint64_t head=0xb0000, body=0xb1000, stop=0xb2000;
    for (bool region : {false,true}) {
        auto e=std::make_unique<Emulator>();
        e->mem().map_range(head,0x3000); e->mem().mprotect_guest(head,0x3000,7);
        const uint32_t entry[]={0xb4010000,0x140003ff}; // cbz x0,stop; b body
        std::vector<uint32_t> code{0x4f00e41e}; // movi v30.16b,#0
        for (unsigned v=0;v<14;++v)
            code.push_back(0x6e201c00u|(v<<16)|(30<<5)|30); // eor v30,v30,vv
        if (region) for (unsigned v=0;v<14;++v)
            code.push_back(0x6e201c00u|(v<<16)|(30<<5)|30); // read again after eviction
        for (unsigned n=14;n>0;--n) {
            const unsigned v=n-1;
            code.push_back(0x4f00e400u|((v+1)<<5)|v); // movi vv.16b,#v+1
        }
        code.push_back(0xd1000400); // sub x0,x0,#1
        code.push_back(region ? 0x14000000u|((uint32_t(-1024-int(code.size())))&0x3ffffff) :
            0xb5000000u|((uint32_t(-int(code.size()))&0x7ffff)<<5));
        if (region) e->mem().write(head,entry,sizeof(entry));
        const uint64_t start=region?body:head;
        e->mem().write(start,code.data(),code.size()*4); e->enable_jit();
        auto* j=e->jit(); if (mt) j->enter_multithreaded();
        auto fn=region?j->compile_tier2_region(*e,j->collect_tier2_trace(*e,head)):
                       j->lookup_or_translate(*e,head);
        if (!fn) return false;
        for (unsigned count : {1u,2u,7u}) {
            CPU actual,reference; actual.pc=head; actual.regs[0]=count;
            for (unsigned v=0;v<32;++v) {
                actual.v_lo[v]=UINT64_C(0x123456789abcdef0)^v;
                actual.v_hi[v]=UINT64_C(0xfedcba9876543210)^(v<<8);
            }
            reference.copy_arch_state_from(actual);
            const uint64_t end=region?stop:start+code.size()*4;
            while (reference.pc!=end) e->step(reference);
            unsigned entries=0;
            while (actual.pc==head && ++entries<=count+2) {
                actual.jit_chain_budget=256; actual.pc=fn(&actual,e.get());
            }
            if (actual.pc!=end || actual.regs[0]!=reference.regs[0] ||
                std::memcmp(actual.v_lo,reference.v_lo,sizeof(actual.v_lo)) ||
                std::memcmp(actual.v_hi,reference.v_hi,sizeof(actual.v_hi))) {
                std::fprintf(stderr,"demand entry carry mismatch: region=%d n=%u\n",region,count);
                return false;
            }
        }
    }
    return true;
}

static bool check_cached_integer_simd(bool mt) {
    constexpr uint64_t head=0x90000, body=0x91000;
    auto e=std::make_unique<Emulator>();
    e->mem().map_range(head,8192); e->mem().mprotect_guest(head,8192,7);
    e->enable_jit(); auto* j=e->jit(); if (mt) j->enter_multithreaded();
    std::vector<uint32_t> operations;
    const uint32_t binary[]={
        0x0e208400,0x2e208400,0x0e209c00, // add, sub, mul
        0x2e206c00,0x2e206400,0x0e206c00,0x0e206400, // u/s min/max
        0x2e208c00,0x0e203400,0x0e203c00,0x2e203400,0x2e203c00, // eq/gt/ge/hi/hs
    };
    const uint32_t shifts[]={
        0x0f005400,0x2f000400,0x0f000400,0x2f001400,0x0f001400,
        0x2f003400,0x0f003400,0x2f005400,0x2f004400,
    }; // shl, ushr, sshr, usra, ssra, ursra, srsra, sli, sri
    for (unsigned size=0;size<4;++size) for (unsigned q=0;q<2;++q) {
        if (size==3 && !q) continue;
        for (unsigned k=0;k<sizeof(binary)/sizeof(binary[0]);++k) {
            if ((k==2 && (size==0 || size==3)) || (k>=3 && k<=6 && size==3)) continue;
            if ((k==3 || k==4) && size==1) continue; // retained interpreter forms
            if (!j->has_sse41() && ((k==2 && size==2) ||
                ((k==3 || k==4) && size==2) || ((k==5 || k==6) && size!=1) ||
                (k==7 && size==3))) continue;
            if (k>7 && size==3 && !j->has_sse42()) continue;
            operations.push_back(binary[k] | (size<<22) | (q<<30));
        }
        const unsigned bits=8u<<size;
        for (unsigned k=0;k<sizeof(shifts)/sizeof(shifts[0]);++k) {
            if (size==3 && (k==2 || k==4 || k==6)) continue; // retained interpreter forms
            const bool left=k==0 || k==7;
            for (unsigned shift : {left ? 0u : 1u, bits/2, left ? bits-1 : bits}) {
                const unsigned imm=left ? bits+shift : 2*bits-shift;
                operations.push_back(shifts[k] | (imm<<16) | (q<<30));
            }
        }
    }
    const bool cache=Tier2TestAccess::supports_vector_cache(*j) &&
                     !std::getenv("BIFROST_NO_VEC_CACHE");
    for (unsigned prefix : {12u,24u})
    for (uint32_t operation : operations) for (unsigned alias=0;alias<4;++alias)
    for (bool region : {false,true}) {
        // Thirteen vectors exercise high XMMs; twenty-five also require
        // evictions in Tier-1 and regions. The target aliases neither input,
        // Vn, Vm, or both.
        const bool shift=(operation&0x0f000000)==0x0f000000;
        if (shift && alias==3) continue;
        const unsigned dest=alias==0 ? 9 : alias==2 ? 11 : 10;
        const unsigned rm=alias==3 ? 10 : 11;
        std::vector<uint32_t> code;
        for (unsigned v=0;v<prefix;++v)
            code.push_back(0x4e3f8400u | (v<<5) | v); // add vv.4s,vv.4s,v31.4s
        code.push_back(operation | dest | (10<<5) | (shift ? 0 : (rm<<16)));
        code.push_back(0xd1000400); // sub x0,x0,#1: preserve NZCV
        const uint64_t back=body+code.size()*4;
        code.push_back(0xb5000000u | ((uint32_t(((region ? head : body)-back)/4)&0x7ffff)<<5));
        j->invalidate_all();
        const uint32_t entry=0x14000400; // b body
        e->mem().write(head,&entry,4); e->mem().write(body,code.data(),code.size()*4);
        auto fn=region ? j->compile_tier2_region(*e,j->collect_tier2_trace(*e,head)) :
                         j->lookup_or_translate(*e,body);
        const bool expected_cache=cache;
        if (!fn || Tier2TestAccess::vector_cache_active(*j)!=expected_cache ||
            (expected_cache && prefix==12 && Tier2TestAccess::vector_reloads(*j)!=13) ||
            (expected_cache && prefix==24 && !Tier2TestAccess::vector_evictions(*j))) {
            std::fprintf(stderr,"integer SIMD cache boundary: word=%08x region=%d prefix=%u\n",
                code[prefix],region,prefix);
            return false;
        }
        const uint64_t start=region ? head : body, stop=body+code.size()*4;
        for (unsigned count : {1u,7u}) {
            CPU actual,reference; actual.pc=start; actual.regs[0]=count;
            actual.pstate=0xa0000000;
            for (unsigned v=0;v<32;++v) {
                actual.v_lo[v]=UINT64_C(0x80ff017fdeadbeef)^(UINT64_C(0x123456789abcdef)*v);
                actual.v_hi[v]=~actual.v_lo[v];
            }
            reference.copy_arch_state_from(actual);
            while (reference.pc!=stop) e->step(reference);
            {
                FrostJIT::FaultScope scope(j);
                unsigned entries=0;
                while (actual.pc!=stop && ++entries<=count+1) {
                    actual.jit_chain_budget=256; actual.pc=fn(&actual,e.get());
                }
            }
            if (actual.pc!=stop || actual.regs[0]!=reference.regs[0] ||
                ((actual.pstate^reference.pstate)&0xf0000000u) ||
                std::memcmp(actual.v_lo,reference.v_lo,sizeof(actual.v_lo)) ||
                std::memcmp(actual.v_hi,reference.v_hi,sizeof(actual.v_hi))) {
                std::fprintf(stderr,"integer SIMD cache mismatch: word=%08x region=%d n=%u\n",
                    code[prefix],region,count); return false;
            }
        }
    }
    std::printf("cached integer SIMD: %zu forms, aliases and retained loops PASS\n",operations.size());
    return true;
}

static bool check_superblocks(bool mt) {
    constexpr uint64_t head=0x40000,cold=0x41000,stop=0x50000;
    auto e=std::make_unique<Emulator>();
    e->mem().map_range(head,8192); e->mem().mprotect_guest(head,8192,7);
    std::vector<uint32_t> code;
    for (unsigned b=0;b<8;++b) {
        code.push_back(0x4e61d400); // fadd v0.2d,v0.2d,v1.2d
        code.push_back(0x91000421); // add x1,x1,#1
        const uint64_t pc=head+code.size()*4;
        code.push_back(0xb5000000u | ((uint32_t((cold-pc)/4)&0x7ffff)<<5) | (b+2));
    }
    code.push_back(0xd65f03c0);
    const uint32_t ret=0xd65f03c0;
    e->mem().write(head,code.data(),code.size()*4); e->mem().write(cold,&ret,4);
    e->enable_jit(); auto* j=e->jit(); if (mt) j->enter_multithreaded();
    const auto trace=j->collect_tier2_trace(*e,head);
    if (!trace.ok || trace.blocks.size()!=8 || trace.total_insts!=24 ||
        std::string(trace.stop_reason)!="block_cap") {
        std::fputs("eight-block superblock cap failed\n",stderr); return false;
    }
    auto run=[&](unsigned side) {
        CPU c; c.pc=head; c.regs[30]=stop; c.jit_stop_pc=stop;
        c.v_lo[1]=c.v_hi[1]=UINT64_C(0x3ff0000000000000);
        if (side<8) c.regs[side+2]=1;
        unsigned dispatches=0;
        while (c.pc!=stop && ++dispatches<100) c.pc=j->run_block(c,*e);
        const double expected=side<8 ? side+1 : 8;
        uint64_t bits; std::memcpy(&bits,&expected,8);
        return c.pc==stop && c.regs[1]==uint64_t(expected) &&
            c.v_lo[0]==bits && c.v_hi[0]==bits;
    };
    for (unsigned k=0;k<96;++k) if (!run(8)) return false;
    if (!j->tier2_regions.load()) {
        std::fputs("linear superblock did not promote\n",stderr); return false;
    }
    Tier2TestAccess::keep_candidates(*j);
    for (unsigned side=0;side<=8;++side) if (!run(side)) {
        std::fprintf(stderr,"superblock side-exit %u mismatch\n",side); return false;
    }
    // Changing an interior instruction revokes the entire fused path.
    j->invalidate_range(head+5*12,4);
    if (j->lookup_only(head)) {
        std::fputs("superblock interior invalidation failed\n",stderr); return false;
    }
    return true;
}

static bool check_region_fp_memory(bool mt) {
    constexpr uint64_t head=0x70000,body=0x71000,exit=0x72000,data=0x80000;
    auto e=std::make_unique<Emulator>();
    e->mem().map_range(head,0x3000); e->mem().mprotect_guest(head,0x3000,7);
    e->mem().map_range(data,4096);
    const uint32_t entry[]={0xb4010002,0x140003ff};
    // d0 and a GPR updated before its FP store share index zero.
    const uint32_t loop[]={0x91000400,0x1e612800,0xfd000060,0xd1000442,0x17fffbfc};
    e->mem().write(head,entry,sizeof(entry)); e->mem().write(body,loop,sizeof(loop));
    e->enable_jit(); auto* j=e->jit(); if (mt) j->enter_multithreaded();
    auto trace=j->collect_tier2_trace(*e,head); auto fn=j->compile_tier2_region(*e,trace);
    if (!fn) return false;
    CPU c; c.pc=head; c.regs[0]=10; c.regs[2]=3; c.regs[3]=data; c.v_lo[1]=UINT64_C(0x3ff0000000000000);
    FrostJIT::FaultScope scope(j);
    unsigned entries=0;
    while (c.pc==head && ++entries<8) { c.jit_chain_budget=256; c.pc=fn(&c,e.get()); }
    uint64_t stored=0; e->mem().read(data,&stored,sizeof(stored));
    if (c.pc!=exit || c.regs[0]!=13 || c.regs[2] || stored!=UINT64_C(0x4008000000000000)) {
        std::fprintf(stderr,"FP/GPR namespace collision: stored=%llx counter=%llu\n",
            (unsigned long long)stored,(unsigned long long)c.regs[0]); return false;
    }
    return true;
}
int main(int argc, char** argv) {
    bool mt = false, benchmark = false;
    for (int k = 1; k < argc; ++k) {
        mt |= std::string(argv[k]) == "mt";
        benchmark |= std::string(argv[k]) == "benchmark";
    }
    if (!check_demand_entry_carry(mt) || !check_allocation_definitions(mt) || !check_cached_integer_simd(mt) || !check_tier1_vector_allocation(mt) ||
        !check_region_invariant(mt) || !check_region_vectors(mt,benchmark) ||
        !check_region_native_simd(mt)) return 1;
    if (!check_region_fp_memory(mt)) return 1;
    if (!benchmark && !check_superblocks(mt)) return 1;
    constexpr uint64_t head = 0x10000, body = 0x11000, exit = 0x12000, stop = 0x20000;
    auto e = std::make_unique<Emulator>();
    e->mem().map_range(head, 0x3000);
    e->mem().mprotect_guest(head, 0x3000, 7);
    const uint32_t entry[] = {0xb4010000, 0x140003ff}; // cbz x0,exit; b body
    std::vector<uint32_t> code(40, 0x91000421); // add x1,x1,#1
    code.push_back(0xd1000400); // sub x0,x0,#1
    const uint64_t branch_pc = body + code.size() * 4;
    code.push_back(0x14000000 | (static_cast<uint32_t>((head - branch_pc) / 4) & 0x3ffffff));
    const uint32_t tail[] = {0xaa0103e0, 0xd65f03c0}; // mov x0,x1; ret
    e->mem().write(head, entry, sizeof(entry));
    e->mem().write(body, code.data(), code.size() * 4);
    e->mem().write(exit, tail, sizeof(tail));
    e->enable_jit();
    auto* j = e->jit();
    if (!j) return 2;
    if (mt) j->enter_multithreaded();
    // Collection is quiescent: no CPU is executing or mutating the table.
    const auto trace = j->collect_tier2_trace(*e, head);
    if (!benchmark && (!trace.ok || trace.blocks.size() != 4 || trace.total_insts != 44 ||
        trace.blocks[2].pc != body || trace.blocks[3].pc != body + 128)) {
        std::fprintf(stderr, "bad trace: ok=%d blocks=%zu instructions=%llu\n",
            trace.ok, trace.blocks.size(), (unsigned long long)trace.total_insts);
        return 1;
    }
    auto run = [&](uint64_t n, uint64_t per_iteration) {
        CPU cpu;
        cpu.pc = head; cpu.regs[0] = n; cpu.regs[1] = 7;
        cpu.regs[30] = stop; cpu.jit_stop_pc = stop;
        unsigned dispatches = 0;
        while (cpu.pc != stop && ++dispatches < 100000)
            cpu.pc = j->run_block(cpu, *e);
        if (cpu.pc != stop || cpu.regs[0] != n * per_iteration + 7) {
            std::fprintf(stderr, "bad result: n=%llu got=%llu pc=%llx\n",
                (unsigned long long)n, (unsigned long long)cpu.regs[0],
                (unsigned long long)cpu.pc);
            return false;
        }
        return true;
    };
    if (!run(2000, 40) || !run(0, 40) || !run(1, 40)) return 1;
    if (benchmark) {
        const auto begin = std::chrono::steady_clock::now();
        if (!run(2000000, 40)) return 1;
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();
        std::printf("two million iterations: %.3f ms; regions=%llu\n", ms,
            (unsigned long long)j->tier2_regions.load());
        return 0;
    }
    if (j->tier2_regions.load() != 1) {
        std::fputs("expected one region, without rotated duplicates\n", stderr); return 1;
    }
    j->invalidate_range(body, 4);
    if (j->lookup_only(head)) {
        std::fputs("non-head invalidation retained region\n", stderr); return 1;
    }
    const uint32_t add_two = 0x91000821;
    e->mem().write(body, &add_two, 4);
    if (!run(2000, 41) || !run(0, 41)) return 1;
    j->invalidate_all();
    if (!run(2000, 41)) return 1;
    // A cycle to a non-head block must stop collection, never hang or
    // silently turn that cycle into a head back-edge.
    const uint32_t self_jump = 0x14000000;
    e->mem().write(body, &self_jump, 4);
    const auto cycle = j->collect_tier2_trace(*e, head);
    if (!cycle.stop_reason || std::string(cycle.stop_reason) != "revisit" ||
        j->compile_tier2_region(*e, cycle)) {
        std::fputs("non-head cycle was not rejected\n", stderr); return 1;
    }
    // Train the taken path independently for each conditional family.
    // The cold path must remain callable after the hot path was linearized.
    for (uint32_t branch : {0xb5000102u, 0x37000102u, 0x54000101u}) {
        j->invalidate_all();
        std::vector<uint32_t> adaptive(13, 0xd503201f); // nop
        adaptive[0] = 0xf100005f; // cmp x2,#0
        adaptive[1] = branch;     // cbnz/tbnz/b.ne x2, head+36
        adaptive[2] = 0x91019021; // cold: add x1,x1,#100
        adaptive[3] = 0x14000007; // b head+40
        adaptive[9] = 0x91000421; // hot: add x1,x1,#1
        adaptive[10] = 0xd1000400; // sub x0,x0,#1
        adaptive[11] = 0xb5fffea0; // cbnz x0,head
        adaptive[12] = 0xd65f03c0; // ret
        e->mem().write(head, adaptive.data(), adaptive.size()*4);
        auto adaptive_run = [&](uint64_t bit, uint64_t iterations) {
            CPU c; c.pc=head; c.regs[0]=iterations; c.regs[2]=bit;
            c.regs[30]=stop; c.jit_stop_pc=stop;
            unsigned dispatches=0;
            while (c.pc != stop && ++dispatches < 10000) c.pc=j->run_block(c,*e);
            return c.pc==stop && c.regs[1]==iterations*(bit ? 1 : 100);
        };
        if (!adaptive_run(1,20)) {
            std::fputs("adaptive branch result mismatch\n",stderr); return 1;
        }
        auto hot = j->collect_tier2_trace(*e,head);
        if (!hot.ok || !hot.fully_profiled || !hot.blocks[0].follows_taken ||
            hot.blocks[1].pc != head+36 || hot.blocks[0].side_exits[0].first != head+8) {
            std::fprintf(stderr,"taken successor was not selected: ok=%d profile=%d taken=%d stop=%s blocks=%zu\n",hot.ok,hot.fully_profiled,hot.blocks[0].follows_taken,hot.stop_reason,hot.blocks.size()); return 1;
        }
        const auto regions_before = j->tier2_regions.load();
        if (!adaptive_run(1,2000) || !adaptive_run(0,7) ||
            j->tier2_regions.load() <= regions_before) {
            std::fputs("adaptive promoted exit mismatch\n",stderr); return 1;
        }
        Tier2TestAccess::keep_candidates(*j);
        const auto retired_before = j->tier2_phase_retirements.load();
        if (!adaptive_run(0,4000)) {
            std::fputs("phase change result mismatch\n",stderr); return 1;
        }
        auto changed = j->collect_tier2_trace(*e,head);
        if (j->tier2_phase_retirements.load() <= retired_before ||
            !changed.fully_profiled || changed.blocks[0].follows_taken) {
            std::fprintf(stderr,"phase did not retrain: retired=%llu before=%llu profile=%d taken=%d\n",
                (unsigned long long)j->tier2_phase_retirements.load(),
                (unsigned long long)retired_before,changed.fully_profiled,changed.blocks[0].follows_taken);
            return 1;
        }
        // Repeated phase flips must keep exact results and eventually stop
        // speculative churn instead of compiling a new region indefinitely.
        for (unsigned phase=0; phase<4; ++phase)
            if (!adaptive_run(phase & 1,4000)) {
                std::fputs("oscillating phase result mismatch\n",stderr); return 1;
            }
        const auto settled = j->tier2_regions.load();
        if (!adaptive_run(0,4000) || !adaptive_run(1,4000) ||
            j->tier2_regions.load() != settled) {
            std::fputs("phase recompilation did not settle\n",stderr); return 1;
        }
        // Profiles from the old mapping must not steer rewritten code.
        j->invalidate_range(head,adaptive.size()*4);
        auto fresh=j->collect_tier2_trace(*e,head);
        if (fresh.fully_profiled || fresh.blocks[0].follows_taken) {
            std::fputs("stale branch profile survived invalidation\n",stderr); return 1;
        }
    }
    // Evenly alternating branches give roughly 50% probability of
    // finishing a whole candidate iteration. Hotness alone must not promote.
    j->invalidate_all();
    const uint32_t weak[] = {0xb5000082, 0x91000421, 0x14000003, 0xd503201f,
        0x91000821, 0xca030042, 0xd1000400, 0xb5ffff20, 0xd65f03c0};
    e->mem().write(head,weak,sizeof(weak));
    CPU weak_cpu; weak_cpu.pc=head; weak_cpu.regs[0]=2000; weak_cpu.regs[3]=1;
    weak_cpu.regs[30]=stop; weak_cpu.jit_stop_pc=stop;
    unsigned weak_dispatches=0;
    const auto rejections_before=j->tier2_profit_rejections.load();
    while (weak_cpu.pc!=stop && ++weak_dispatches<10000)
        weak_cpu.pc=j->run_block(weak_cpu,*e);
    if (weak_cpu.pc!=stop || weak_cpu.regs[1]!=3000 ||
        j->tier2_profit_rejections.load()<=rejections_before) {
        std::fprintf(stderr,"weak path profitability check failed: pc=%llx sum=%llu rejects=%llu before=%llu\n",(unsigned long long)weak_cpu.pc,(unsigned long long)weak_cpu.regs[1],(unsigned long long)j->tier2_profit_rejections.load(),(unsigned long long)rejections_before); return 1;
    }
    // More loop-carried GPRs than host pins. SSA head phis must see each
    // preceding iteration's writes, including deferred non-pinned stores.
    j->invalidate_all();
    std::vector<uint32_t> carried;
    for (unsigned a = 1; a <= 12; ++a)
        carried.push_back(0x91000000u | (a << 10) | (a << 5) | a);
    carried.push_back(0xd1000400); // sub x0,x0,#1
    const int64_t back = -static_cast<int64_t>(carried.size());
    carried.push_back(0xb5000000u | ((static_cast<uint32_t>(back) & 0x7ffff) << 5));
    carried.push_back(0xd65f03c0);
    e->mem().write(head, carried.data(), carried.size() * 4);
    const auto carried_trace = j->collect_tier2_trace(*e, head);
    auto carried_fn = j->compile_tier2_region(*e, carried_trace);
    if (!carried_fn) { std::fputs("carried SSA region did not compile\n", stderr); return 1; }
    for (uint64_t count : {1ULL, 2ULL, 63ULL, 2000ULL}) {
        CPU c; c.pc = head; c.regs[0] = count; c.regs[30] = stop; c.jit_stop_pc = stop;
        for (unsigned a = 1; a <= 12; ++a) c.regs[a] = a * 0x1000;
        unsigned entries = 0;
        const uint64_t after_loop = head + (carried.size() - 1) * 4;
        while (c.pc == head && ++entries <= count + 1) c.pc = carried_fn(&c, e.get());
        if (c.pc != after_loop || c.regs[0]) {
            std::fputs("carried SSA loop exit mismatch\n", stderr); return 1;
        }
        for (unsigned a = 1; a <= 12; ++a)
            if (c.regs[a] != a * (0x1000 + count)) {
                std::fprintf(stderr, "carried SSA x%u count=%llu got=%llu\n", a,
                    (unsigned long long)count, (unsigned long long)c.regs[a]); return 1;
            }
    }
    // A fault in nested call dispatch must become a guest signal before
    // it can unwind through a generated host frame.
    auto fault_emu=std::make_unique<Emulator>();
    CPU fault; fault.running=true; fault.regs[30]=stop;
    jit_call_helper(&fault,fault_emu.get(),0xdead0000);
    if (fault.running || fault.exit_code!=139) {
        std::fputs("call fallback did not deliver guest SIGSEGV\n",stderr); return 1;
    }
    std::puts("jit_tier2_discovery: direct edges, capped bodies, exits and invalidation PASS");
}
