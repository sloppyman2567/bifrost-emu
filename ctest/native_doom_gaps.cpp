// All captured gap encodings, randomized register state, memory/writeback,
// exclusive success/failure, and real JIT dispatch vs interpreter execution.
#include "core/emulator.h"
#include "jit/frostjit.hpp"
#include "ir/ir.hpp"
#include <cstdio>
#include <cstring>
#include <random>
using namespace arm64emu;
static const uint32_t opcodes[]={
#include "doom_gap_opcodes.inc"
};
int main() {
 Emulator e; e.mem().map_range(0x10000,0x10000);e.mem().mprotect_guest(0x10000,0x10000,7);
 e.mem().map_range(0x40000,0x1000);e.mem().mprotect_guest(0x40000,0x1000,3);
 e.enable_jit();std::mt19937_64 rng(71235);unsigned checks=0,failures=0,missing=0;
 CPU ref,native;uint8_t initial[128],expected[128],actual[128];
 for(unsigned index=0;index<sizeof(opcodes)/sizeof(opcodes[0]);index++) {
  uint32_t word=opcodes[index];DecodedInst decoded{};decode(decoded,word);
  IRBlock ir{};ir_reset_vreg_alloc();translate_to_ir(ir,decoded,0x10000+index*16);
  for(const auto& inst:ir.insts) if(inst.op==IROp::CALL_INTERP){++missing;std::printf("MISSING %08x\n",word);}
  uint64_t pc=0x10000+index*16;uint32_t code[2]={word,0xd65f03c0u};e.mem().write(pc,code,8);
  bool memory=decoded.cls==InstClass::SIMD_LD1 || decoded.cls==InstClass::SIMD_ST1 || decoded.cls==InstClass::LDXR || decoded.cls==InstClass::STXR || decoded.cls==InstClass::LDAXR || decoded.cls==InstClass::STLXR || decoded.cls==InstClass::LDAR || decoded.cls==InstClass::STLR;
  for(unsigned sample=0;sample<32;sample++) {
   ref.pc=pc;ref.running=true;ref.sp=0x40000;ref.pstate=(sample&15)<<28;ref.excl_clear();
   for(unsigned r=0;r<32;r++){ref.v_lo[r]=rng();ref.v_hi[r]=rng();ref.regs[r]=r==31?0:rng();}
   if(memory && decoded.rn!=31)ref.regs[decoded.rn]=0x40000;
   ref.regs[30]=0x30000;native.copy_arch_state_from(ref);native.excl_clear();
   if(memory && (sample&1)) {ref.excl_mark(0x40000,1<<decoded.size);native.excl_mark(0x40000,1<<decoded.size);}
   for(auto& byte:initial)byte=uint8_t(rng());e.mem().write(0x40000,initial,sizeof(initial));
   e.step(ref);e.step(ref);e.mem().read(0x40000,expected,sizeof(expected));
   auto* shard=reinterpret_cast<Emulator::ExclMonitorShardAccess*>(e.excl_monitor_shard_pub(0x40000));
   {std::lock_guard<std::mutex> lock(shard->mu);shard->reservations.clear();}
   e.mem().write(0x40000,initial,sizeof(initial));
   unsigned blocks=0;while(native.running && native.pc!=0x30000 && blocks++<8)e.jit()->run_block(native,e);
   e.mem().read(0x40000,actual,sizeof(actual));
   bool same=native.pc==ref.pc && native.sp==ref.sp && (native.pstate&0xF0000000)==(ref.pstate&0xF0000000) && !std::memcmp(actual,expected,sizeof(actual));
   for(unsigned r=0;r<32;r++)same=same && native.regs[r]==ref.regs[r] && native.v_lo[r]==ref.v_lo[r] && native.v_hi[r]==ref.v_hi[r];
   if(!same){if(failures++<20){unsigned rd=word&31;std::printf("FAIL %08x sample=%u pc=%llx/%llx v%u=%llx:%llx/%llx:%llx flags=%x/%x\n",word,sample,(unsigned long long)native.pc,(unsigned long long)ref.pc,rd,(unsigned long long)native.v_hi[rd],(unsigned long long)native.v_lo[rd],(unsigned long long)ref.v_hi[rd],(unsigned long long)ref.v_lo[rd],native.pstate,ref.pstate);}}
   {std::lock_guard<std::mutex> lock(shard->mu);shard->reservations.clear();}
   ++checks;
  }
 }
 std::printf("native_doom_gaps: %u checks, %u mismatches, %u IR fallbacks\n",checks,failures,missing);
 return failures||missing?1:0;
}
