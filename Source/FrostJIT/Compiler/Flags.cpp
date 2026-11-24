// Flags.cpp — FrostJIT flag materialization helpers.
//
// split out of FrostJIT.cpp. Holds the
// clobber_flags() and materialize_flags_to_pstate() methods, which
// manage the lazy materialization of guest NZCV flags from host RFLAGS
// to cpu.pstate. These are called by compile_ir_inst (in FrostJIT.cpp
// and Dispatch.cpp) before any instruction that would clobber
// RFLAGS or at block exits.
#include "FrostJIT/Compiler/BlockAnalysis.hpp"
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include <cstdint>
#include <cstring>
namespace arm64emu {
// ── clobber_flags ───────────────────────────────────────────────────────
// If the host RFLAGS currently hold valid guest NZCV (flags_in_host_),
// materialize them to cpu.pstate BEFORE a flag-clobbering instruction
// overwrites them. Without this, any ALU op after ADDS/SUBS/TST would
// lose the flags, causing wrong branch decisions downstream.
void FrostJIT::clobber_flags(bool dead, bool rdx_only) {
    if (flags_in_host_) {
        dead = dead || host_flags_match_pstate();
        // emit_materialize_flags overwrites RDX and RAX (RAX optionally saved,
        // RCX never touched). Callers that go on to pick registers through the
        // allocator can leave RAX/RCX mapped; the rest take all three, as an
        // unspecific caller may use RAX/RCX/RDX as fixed scratch next.
        const uint16_t FLAGS_CLOBBER = rdx_only
            ? static_cast<uint16_t>(1u << RDX)
            : static_cast<uint16_t>((1u << RAX) | (1u << RCX) | (1u << RDX));
        // Spill dirty vregs AND non-dirty scratch vregs BEFORE the
        // materialize — otherwise non-dirty scratch vregs cached in the
        // clobbered registers are lost (their value is only in the host reg,
        // and materialize overwrites it before invalidate_host_regs can drop
        // the mapping). This was the root cause of the FWD crash on
        // `toybox ls /` (x30 store using a stale/garbage value).
        flush_dirty_host_regs(FLAGS_CLOBBER);
        flush_scratch_host_regs(FLAGS_CLOBBER);
        // RAX needs saving only while it holds a mapped vreg (rdx_only leaves
        // the mapping) or the exit's next PC. The full form keeps the save:
        // its callers may stage a value in RAX outside the allocator.
        const bool keep_rax = !rdx_only || rax_holds_next_pc_ || reg_vreg_[RAX] >= 0;
        if (!dead) emit_materialize_flags(flags_from_sub_, keep_rax);
        flags_in_host_ = false;
        // Drop cache mappings for the clobbered registers (values were spilled above).
        invalidate_host_regs(FLAGS_CLOBBER);
    }
}
// Materialize host flags to pstate. Preserve RFLAGS when a later instruction
// still consumes them; BRCOND has already consumed its condition and opts out.
// Its exits load RAX with the next PC right after, so RAX, flushed here, is not
// saved around the pack either. Both branch exits still commit guest NZCV. No-op if flags aren't
// currently in host. Does NOT clear flags_in_host_ — the caller manages that,
// since BRCOND emits this on both paths before clearing flags_in_host_ at the
// end.
void FrostJIT::materialize_flags_to_pstate(bool preserve_flags, bool dead) {
    if (!flags_in_host_) return;
    dead = dead || host_flags_match_pstate();
    constexpr uint16_t FLAGS3 = (1u << RAX) | (1u << RCX) | (1u << RDX);
    if (!dead && preserve_flags) emit_pushfq();
    flush_dirty_host_regs(FLAGS3);
    flush_scratch_host_regs(FLAGS3);
    if (!dead) emit_materialize_flags(flags_from_sub_, preserve_flags);
    if (!dead && preserve_flags) emit_popfq();
    invalidate_host_regs(FLAGS3);
}

// ucomiss/ucomisd a, b leave: greater CF=0 ZF=0, less CF=1 ZF=0, equal ZF=1, unordered CF=ZF=PF=1 (SF=OF=0 always). Read
// as the ARM outcome of fcmp a, b (greater N=0 Z=0 C=1 V=0, less N=1, equal Z=1 C=1, unordered C=1 V=1), every condition
// but EQ/NE is one jcc on those flags, with the operands swapped (ucomis b, a) where the plain order would need two:
//   a, b order:   GE jae, LT jb, GT ja, LE jbe, VS jp, VC jnp
//   b, a order:   HI jb, LS jae, MI ja, PL jbe, CS jbe, CC ja
// EQ is ZF and not PF and NE the reverse: two jumps, so those keep the SUB-convention sequence.
bool FrostJIT::fp_raw_flags_cc(uint8_t arm_cond, bool& swapped, uint8_t& cc) {
    swapped = false;
    switch (arm_cond & 0xF) {
        case 0xA: cc = 0x3; return true;                  // GE
        case 0xB: cc = 0x2; return true;                  // LT
        case 0xC: cc = 0x7; return true;                  // GT
        case 0xD: cc = 0x6; return true;                  // LE
        case 0x6: cc = 0xA; return true;                  // VS
        case 0x7: cc = 0xB; return true;                  // VC
        case 0x8: swapped = true; cc = 0x2; return true;  // HI
        case 0x9: swapped = true; cc = 0x3; return true;  // LS
        case 0x4: swapped = true; cc = 0x7; return true;  // MI
        case 0x5: swapped = true; cc = 0x6; return true;  // PL
        case 0x2: swapped = true; cc = 0x6; return true;  // CS
        case 0x3: swapped = true; cc = 0x7; return true;  // CC
        default: return false;
    }
}
// Two small integers whose `cmp` leaves the flags a subtraction would give for each ARM outcome (SF=N, ZF=Z, CF=!C,
// OF=V): greater 1-0, equal 0-0, less 0-1, unordered 0x80000000-1 (signed overflow). mov and cmov keep the ucomis flags.
// In the swapped order (ucomis b, a) "greater" is CF=1 and "less" is CF=0 and ZF=0.
void FrostJIT::emit_fp_raw_flags_to_sub(bool swapped) {
    emit_mov_imm32_zext(RAX, 0);
    emit_byte(0x0F); emit_byte(swapped ? 0x92 : 0x97); emit_byte(0xC0);   // setb al (swapped) / seta al
    emit_mov_imm32_zext(RCX, 0);
    emit_byte(0x0F); emit_byte(swapped ? 0x97 : 0x92); emit_byte(0xC1);   // seta cl (swapped) / setb cl
    if (swapped) {
        emit_mov_imm32_zext(RDX, 1);
        emit_byte(0x0F); emit_byte(0x4A); emit_byte(0xCA);                // cmovp ecx, edx: unordered b = 1
    }
    emit_mov_imm32_zext(RDX, 0x80000000);
    emit_byte(0x0F); emit_byte(0x4A); emit_byte(0xC2);                    // cmovp eax, edx: unordered a = INT_MIN
    emit_byte(0x39); emit_byte(0xC8);                                     // cmp eax, ecx
}
void FrostJIT::materialize_fp_raw_flags_to_pstate(bool dead) {
    constexpr uint16_t FLAGS3 = (1u << RAX) | (1u << RCX) | (1u << RDX);
    flush_dirty_host_regs(FLAGS3);
    flush_scratch_host_regs(FLAGS3);
    if (!dead) {
        emit_fp_raw_flags_to_sub(fp_raw_swapped_);
        emit_materialize_flags(true, false);
    }
    invalidate_host_regs(FLAGS3);
}

bool FrostJIT::exit_flags_dead(uint64_t pc) {
    if (!flag_scan_active_ || !window_base_ || !guest_memory_) return false;
    for (const auto& dep : flag_deps_scratch_)
        if (dep.pc == pc) return true;  // only dead verdicts are recorded
    // Read the words directly, only from pages that are plainly mapped and
    // executable: this is a prediction about code that may never run, so it
    // must not populate a missing page or raise a fault.
    constexpr int kWords = jit_analysis::kNzcvScanWords;
    constexpr uint64_t kBytes = kWords * 4;
    const uint64_t window = guest_memory_->window_size();
    const uint64_t at = fetch_address(pc);  // the shifted proof compile reads the real words
    if ((at & 3) || at > window || kBytes > window - at) return false;
    const auto* flags = guest_memory_->direct_page_flags();
    constexpr uint8_t need = Memory::GUEST_PAGE_MAPPED | Memory::GUEST_PROT_EXEC;
    for (uint64_t p = at / Memory::PAGE_SIZE; p <= (at + kBytes - 1) / Memory::PAGE_SIZE; ++p) {
        const uint8_t f = flags[p].load(std::memory_order_acquire);
        if ((f & need) != need || (f & Memory::GUEST_PAGE_UFFD_MISSING)) return false;
    }
    uint32_t words[kWords];
    std::memcpy(words, window_base_ + at, sizeof words);
    int used = 0;
    if (!jit_analysis::nzcv_dead_in(words, kWords, used) || used <= 0) return false;
    flag_deps_scratch_.push_back({pc, std::vector<uint32_t>(words, words + used)});
    flag_store_skipped_ = true;
    return true;
}
// A growth hint only: the words it reads are translated (and recorded) only if
// the block grows, and a block that does not grow depends on none of them.
// Like exit_flags_dead it reads plainly mapped executable pages directly, so a
// prediction never populates a missing page or raises a fault.
int FrostJIT::selfloop_extent(uint64_t start_pc, uint64_t pc, int count, int limit) {
    if (!window_base_ || !guest_memory_ || count >= limit) return 0;
    const uint64_t window = guest_memory_->window_size();
    const auto* flags = guest_memory_->direct_page_flags();
    constexpr uint8_t need = Memory::GUEST_PAGE_MAPPED | Memory::GUEST_PROT_EXEC;
    for (int n = count; n < limit; ++n, pc += 4) {
        const uint64_t at = fetch_address(pc);
        if ((at & 3) || at > window - 4) return 0;
        const uint8_t f = flags[at / Memory::PAGE_SIZE].load(std::memory_order_acquire);
        if ((f & need) != need || (f & Memory::GUEST_PAGE_UFFD_MISSING)) return 0;
        uint32_t word;
        std::memcpy(&word, window_base_ + at, sizeof word);
        DecodedInst d;
        if (!decode(d, word) || jit_analysis::instr_will_call_interp(d)) return 0;
        switch (d.cls) {
            case InstClass::B: case InstClass::Bcond: case InstClass::CBZ: case InstClass::CBNZ:
            case InstClass::TBZ: case InstClass::TBNZ:
                return pc + static_cast<uint64_t>(d.imm) == start_pc ? n + 1 : 0;
            case InstClass::BL: case InstClass::BLR: case InstClass::BR: case InstClass::RET:
            case InstClass::SVC: case InstClass::BRK: case InstClass::UNKNOWN:
                return 0;
            default:
                break;
        }
    }
    return 0;
}
// ── Block chaining helpers ──────────────────────────────────────────────
// Patch a block's 5-byte chain slot (originally `ret` + 4 NOPs) in place
// to `jmp rel32` → target_fn. x86 is icache-coherent, so no explicit
// cache flush is needed, but we emit a memory barrier to ensure the
// patched bytes are visible to any in-flight execution on the same core.
// single source of truth for "will this instruction route
// to CALL_INTERP in the IR translator?" Used by the block splitter to
// pre-scan before translating. If this list gets out of sync with
// Integer.cpp's default case, the splitter would misclassify
// instructions.
//
// Note: SIMD_LD1, SIMD_ST1, SIMD_DUP, and SIMD_LOGICAL have native IR
// paths (IROp::SIMD_LDST / SIMD_DUP / SIMD_LOGICAL) and do NOT route to
// CALL_INTERP for the common cases — they are deliberately NOT listed
// here so the block splitter doesn't fragment blocks around them.
// SIMD_LOGICAL falls back to CALL_INTERP only for unrecognized opcodes
// (its `default:` case), but that's rare enough to accept the risk.
//
// instr_will_call_interp() was moved to
// BlockCompiler.cpp (it's only used by translate_block).
} // namespace arm64emu
