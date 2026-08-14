// jit/jit_translate.cpp — FrostJIT block translation.
//
// v1.4.5-alpha: split out of frostjit.cpp. Holds the
// translate_block() method, which translates one ARM64 basic block to
// x86-64 code. The method:
//   1. Decodes ARM64 instructions via the shared decoder.
//   2. Translates each to IR ops (ir_translate.cpp).
//   3. Optimizes the IR (ir_optimize.cpp).
//   4. Compiles each IR op to x86 via compile_ir_inst() (frostjit.cpp
//      + jit_codegen_fp.cpp).
//   5. Patches branch targets and block-chaining slots.
//   6. Returns a function pointer to the compiled block.
#include "jit/frostjit.hpp"
#include "core/emulator.h"
#include "frontend/dynamic_linker.h"
#include "ir/ir.hpp"
#include "opgen_simd.hpp"
#include "opgen_fpfixed.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>
namespace arm64emu {
// BIFROST_PROF sampling-flag toggle (see jit_glue.cpp).
extern thread_local bool prof_in_translate;
bool bifrost_prof_active();
struct ProfTranslateGuard {
    bool saved_;
    ProfTranslateGuard() : saved_(bifrost_prof_active()) { if (saved_) prof_in_translate = true; }
    ~ProfTranslateGuard() { if (saved_) prof_in_translate = false; }
};
// (end block) instead of BL_CALL (call within block).
extern thread_local bool bl_call_disabled_;
// ── instr_will_call_interp — heuristic for block splitting ─────────────
// Returns true if the given ARM64 instruction is likely to generate a
// CALL_INTERP IR op (i.e. the JIT can't codegen it natively). Used by
// translate_block to decide where to split blocks — too many
// CALL_INTERP fallbacks in one block causes register pressure issues.
//
// v1.4.5-alpha: moved here from jit_flags.cpp (where it was
// extracted by accident — it's only used by translate_block).
static bool instr_will_call_interp(const DecodedInst& d) {
    switch (d.cls) {
        case InstClass::SIMD_DP: {
            // Native SIMD_DP ops are classified by the generated table
            // (arm64emu::simd::classify, from tools/opgen/simd_dp.txt).
            // Anything the table doesn't recognize falls back to the
            // interpreter. Previously this was a hand-written set of
            // sub3_noq/fp_key/sm masks mirroring ir_translate_fp.cpp (and
            // silently drifting — AND/ORR/EOR/DUP native paths were missing,
            // forcing those blocks down the interpreter).
            return simd::classify(d.raw).family == simd::Family::UNKNOWN;
        }
        case InstClass::FP_SCALAR: {
            uint32_t op = d.raw;
            uint8_t ftype = (op >> 22) & 3;
            // Mirror ir_translate_fp.cpp's FP_SCALAR handling: nearly all
            // scalar FP ops translate to native IR (FP_BINOP/FP_UNOP/FP_CMP/
            // FCVT/FP_F2I/FP_I2F/FMADD/FCSEL/FRINT/FP_MOVI/FMOV). Only
            // half-precision (ftype==3) and unmatchable encodings fall back
            // to CALL_INTERP. Previously this returned true for EVERY
            // FP_SCALAR, so FP-heavy blocks (e.g. the voxel game's
            // transform/render math) tripped the interp_only heuristic and
            // ran the interpreter — ~92% of dispatches never hit JIT code.
            //
            // BIFROST_FP_NATIVE_GATE=<bitmask> (debug/bisect): when set,
            // only FP groups whose bit is set are treated as native; all
            // others fall back to CALL_INTERP. Bitmap:
            //   0x01 FMOV GPR<->FP / FMOV FP<->FP / FMOV imm
            //   0x02 FCMP/FCMPE
            //   0x04 FP 2-source (FADD/FSUB/FMUL/FDIV/FMAX/FMIN/FMAXNM/FMINNM/FNMUL) + FABD
            //   0x08 FP 1-source (FABS/FNEG/FSQRT/FRINT*)
            //   0x10 FCVTZS/FCVTZU (FP->int)
            //   0x20 FCVT D<->S + SCVTF/UCVTF (int->FP)
            //   0x40 FMA family
            //   0x80 FCSEL
            static int fp_gate = [] {
                const char* s = getenv("BIFROST_FP_NATIVE_GATE");
                return s ? static_cast<int>(strtol(s, nullptr, 0)) : -1;  // -1 = all native
            }();
            // FMOV GPR↔FP (32/64-bit) — ftype-agnostic in the translator.
            if ((op & 0xFFE0FC00) == 0x9E600000 && (op & (1u << 18)) && (op & (1u << 17)))
                return fp_gate < 0 || (fp_gate & 0x01);
            if ((op & 0xFFE0FC00) == 0x1E200000 && (op & (1u << 18)) && (op & (1u << 17)))
                return fp_gate < 0 || (fp_gate & 0x01);
            if (ftype > 1)
                return true;  // half-precision → interpreter
            // FMOV FP↔FP (double 0x1E604000 / single 0x1E204000).
            if ((op & 0xFFFFFC00) == 0x1E604000 || (op & 0xFFFFFC00) == 0x1E204000)
                return fp_gate < 0 || (fp_gate & 0x01);
            // FMOV (scalar, immediate).
            if (fp_decode::is_fmov_imm(op))
                return fp_gate < 0 || (fp_gate & 0x01);
            // FCMP/FCMPE (register or #0.0 form).
            if (fp_decode::is_fcmp(op))
                return fp_gate < 0 || (fp_gate & 0x02);
            // FP 2-source (FADD/FSUB/FMUL/FDIV/FMAX/FMIN/FMAXNM/FMINNM/FNMUL).
            if (((op >> 21) & 1) == 1 && ((op >> 10) & 0x3) == 0b10
                && (op & 0xFF000000) == 0x1E000000 && ((op >> 12) & 0xF) <= 8)
                return fp_gate < 0 || (fp_gate & 0x04);
            // FABD (scalar/vector, S/D via bit22).
            if ((op & 0xFF00FC00) == 0x7E00D400)
                return fp_gate < 0 || (fp_gate & 0x04);
            // FP 1-source: FABS/FNEG/FSQRT (1..3) and FRINT* (0x08..0x0F).
            if (fp_decode::is_fp_1source(op)) {
                uint8_t fp1 = fp_decode::fp_1source_opcode(op);
                if ((fp1 >= 1 && fp1 <= 3) || (fp1 >= 0x08 && fp1 <= 0x0F))
                    return fp_gate < 0 || (fp_gate & 0x08);
            }
            // FCVTZS/FCVTZU (FP→int toward zero).
            if ((op & 0x7F3E0000) == 0x1E380000)
                return fp_gate < 0 || (fp_gate & 0x10);
            // FCVT D↔S (0x1E624000 double→single, 0x1E22C000 single→double).
            if ((op & 0xFFFFFC00) == 0x1E624000 || (op & 0xFFFFFC00) == 0x1E22C000)
                return fp_gate < 0 || (fp_gate & 0x20);
            // SCVTF/UCVTF (int→FP).
            if ((op & 0x7F3EFC00) == 0x1E220000)
                return fp_gate < 0 || (fp_gate & 0x20);
            // Fixed-point int↔FP converts (SCVTF/UCVTF/FCVTZS/FCVTZU #fbits).
            // Classified via the generated table (tools/opgen/fp_fixconv.txt)
            // so this gate can't drift from interp/IR. Covers the FPDataProc1
            // forms (GPR source/dest) AND the AdvSIMD-scalar forms with FP
            // register source/dest (e.g. GCC's `scvtf s0, s0, #1`).
            if (fpfixed::classify(op).family == fpfixed::Family::FIXCONV)
                return fp_gate < 0 || (fp_gate & 0x20);
            // FMA family (FMADD/FMSUB/FNMADD/FNMSUB).
            if ((op & 0xFF000000) == 0x1F000000)
                return fp_gate < 0 || (fp_gate & 0x40);
            // FCSEL.
            if ((op & 0xFF200C00) == 0x1E200C00)
                return fp_gate < 0 || (fp_gate & 0x80);
            return true;  // rare/unsupported scalar FP → interpreter
        }
        case InstClass::LDXR: case InstClass::STXR:
        case InstClass::LDAXR: case InstClass::STLXR:
        case InstClass::LDAR: case InstClass::STLR:
            return true;
        default:
            break;
    }
    // v1.5.0.alpha: SIMD&FP LDR/STR (B/H/S/D/Q) and SIMD LDP/STP are all
    // natively translated (ir_translate_mem.cpp) — the old is_vec gate
    // here forced every one of them to CALL_INTERP, splitting FP-heavy
    // blocks every 1-2 instructions and killing the pinned-XMM vec cache
    // (each FP load/store cost ~18 interpreter steps in the voxel game).
    return false;
}
// ── translate_block ───────────────────────────────────────────────
uint64_t (*FrostJIT::translate_block(Emulator& emu, uint64_t start_pc))(CPU*, Emulator*) {
    ProfTranslateGuard prof_g;
    if (!code_buf_) return nullptr;
    // W^X: toggle the code buffer to writable before emitting x86 code.
    // (No-op if W^X is disabled or the buffer is already writable.)
    make_writable();
    current_start_pc_ = start_pc;
    code_buf_overflow_ = false;
    call_interp_branch_patches_.clear();
    branch_target_patches_.clear();
    rax_holds_next_pc_ = false;
    flags_in_host_ = false;
    flags_from_sub_ = false;
    chain_target_pc_ = 0;
    unchainable_end_ = false;
    has_selfloop_slot_ = false;
    selfloop_patch_off_ = 0;
    has_taken_chain_slot_ = false;
    taken_chain_patch_off_ = 0;
    taken_chain_target_pc_ = 0;
    num_stack_slots_ = 0;
    vec_cache_reset();
    // Only clear the vreg arrays up to the previous block's max_vreg_+1,
    // not all 4096 entries. This saves ~12KB of writes per block
    // translation when blocks are small (typical: max_vreg_ ≈ 33-100).
    int clear_limit = prev_max_vreg_ + 1;
    if (clear_limit > 4096) clear_limit = 4096;
    for (int i = 0; i < clear_limit; i++) {
        vreg_home_[i] = -1;
        vreg_dirty_[i] = false;
        vreg_slot_[i] = 0;
        vreg_last_use_[i] = 0;  // reset LRU timestamps
    }
    for (int i = 0; i < NUM_HOST_REGS; i++) reg_vreg_[i] = -1;
    max_vreg_ = 0;
    dirty_host_regs_ = 0;  // reset dirty-bitmask
    size_t block_start = code_buf_used_;
    // ── Translate ARM64 → IR ─────────────────────────────────────
    IRBlock ir_block;
    ir_block.start_pc = start_pc;
    ir_reset_vreg_alloc();
    // Limit block size based on register pressure. With 9 host regs and
    // >256 vregs, the allocator's spill/reload traffic becomes a
    // correctness hazard. Cap blocks at 32 instructions — enough for
    // tight loops, short enough that vreg count stays manageable.
    constexpr int MAX_BLOCK_REG_PRESSURE = 32;
    // limit the number of CALL_INTERP fallbacks per
    // block. Each CALL_INTERP invalidates all cached vregs, and each
    // subsequent clobber_flags() drops non-dirty vregs from RAX/RCX/RDX.
    // In long blocks with many CALL_INTERPs (e.g., __multf3's 82-instr
    // multiply block with ~10 CSINC/CCMP/CSEL fallbacks), this creates
    // a cascade of stale reloads that corrupt register values.
    //
    // Fix: after MAX_CALL_INTERP_PER_BLOCK interpreter fallbacks, force
    // a block boundary. The next instruction becomes the start of a new
    // block, which gets a fresh register allocator state. This trades a
    // small perf cost (one extra block dispatch per split) for
    // correctness in complex blocks.
    constexpr int MAX_CALL_INTERP_PER_BLOCK = 2;
    // invalidates all cache mappings, so too many in one block kills perf.
    // Cap at 2 — enough for small function call sequences, low enough to
    // keep register pressure manageable and avoid excessive vreg flushes.
    constexpr int MAX_BL_CALL_PER_BLOCK = 2;
    int call_interp_count = 0;
    int bl_call_count = 0;
    uint64_t cur_pc = start_pc;
    int instr_count = 0;
    bool block_ended = false;
    while (!block_ended && instr_count < MAX_BLOCK_REG_PRESSURE) {
        // ── Block splitting at known entry points ──────────────────
        if (instr_count > 0 && blocks_.find(cur_pc) != blocks_.end()) {
            block_profile.entry_point++;
            chain_target_pc_ = cur_pc;
            break;
        }
        uint32_t inst;
        try {
            inst = emu.mem().fetch_inst(cur_pc);
        } catch (...) { block_profile.decode_fail++; break; }
        DecodedInst d;
        if (!decode(d, inst)) { block_profile.decode_fail++; break; }
        bool will_call_interp = instr_will_call_interp(d);
        if (will_call_interp && call_interp_count >= MAX_CALL_INTERP_PER_BLOCK && instr_count > 0) {
            // Split here — the next instruction starts a new block.
            block_profile.call_interp_cap++;
            chain_target_pc_ = cur_pc;
            break;
        }
        bool ends = translate_to_ir(ir_block, d, cur_pc);
        if (will_call_interp) call_interp_count++;
        // Track BL_CALL count for block splitting.
        if (d.cls == InstClass::BL) bl_call_count++;
        instr_count++;
        ir_block.count = instr_count;
        if (ends) {
            block_profile.natural_branch++;
            block_ended = true;
        }
        else {
            if (bl_call_count >= MAX_BL_CALL_PER_BLOCK) {
                block_profile.bl_call_cap++;
                chain_target_pc_ = cur_pc + 4;
                break;
            }
            cur_pc += 4;
        }
    }
    if (instr_count >= MAX_BLOCK_REG_PRESSURE) block_profile.max_size++;
    if (instr_count == 0) {
        make_executable();  // W^X: balance the make_writable() at entry
        return nullptr;
    }
    // ── glibc malloc/free freelist quarantine (Track A) ────────────
    // The JIT has a known register-state corruption entering glibc's
    // malloc/free safe-linked freelist code (PROTECT_PTR head reads as 0,
    // corrupting the heap). Blocks inside glibc's allocator hot region
    // run via the interpreter, which is the correct baseline. The region
    // is matched by name + file offset so it survives ASLR.
    static const bool qt_quarantine_ = (getenv("BIFROST_MALLOC_INTERP") != nullptr);
    if (qt_quarantine_) {
        if (emu.dyn_linker()) {
            uint64_t off = emu.dyn_linker()->object_relative_offset(start_pc, "libc");
            // malloc/free/calloc/realloc + internal _int_malloc/_int_free.
            if (off != ~uint64_t(0) && off >= 0x90480 && off <= 0x92800) {
                BlockEntry entry;
                entry.fn = nullptr;
                entry.interp_only = true;
                entry.interp_only_count = instr_count;
                entry.ends_with_branch = ir_block.ends_with_branch;
                entry.chain_target_pc = 0;
                entry.chained = false;
                entry.instr_count = instr_count;
                entry.verified_once = true;  // interp-only: no verify
                blocks_[start_pc] = entry;
                blocks_translated++;
                block_profile.interp_only++;
                make_executable();  // W^X: balance the make_writable() at entry
                return nullptr;
            }
        }
    }
    // ── Heuristic: skip JIT for CALL_INTERP-heavy blocks ──────────
    // The JIT's per-CALL_INTERP overhead (flush all vregs + push 2 regs +
    // call interpreter + pop 2 regs + reload) is ~20 instructions. For
    // blocks where CALL_INTERP dominates (more than half the instructions
    // are interpreter fallbacks), the pure interpreter is faster — it
    // skips the prologue/epilogue/dispatch entirely.
    //
    // refined from "any CALL_INTERP → interp_only" to
    // "CALL_INTERP-heavy → interp_only". The old heuristic was too
    // aggressive — a block with 10 native ops and 1 CALL_INTERP would
    // skip JIT entirely, losing the 10 native ops' speedup. Now we only
    // fall back to interpreter when CALL_INTERP is the majority.
    //
    // This fixes the long-double multiply/divide hang: __multf3/__divtf3
    // are ~82-instruction soft-float routines split into ~20 tiny blocks
    // by the MAX_CALL_INTERP_PER_BLOCK=2 splitter. The JIT was 100-200x
    // slower than the interpreter for these, causing effective hangs on
    // jit_block_split.elf and jit_fp_scalar.elf with --jit.
    //
    // Interp-only blocks are cached (so we skip the re-decode cost on
    // cache hits) and run exactly instr_count interpreter steps.
    // blocks with >32 instructions have too much
    // register pressure for the 9-host-reg allocator. The __multf3
    // 82-instruction softfloat block generates ~246 vregs, causing
    // spill/reload correctness bugs. Run long blocks via interpreter.
    if (instr_count > 32 ||
        (call_interp_count > 0 && call_interp_count * 2 > instr_count)) {
        BlockEntry entry;
        entry.fn = nullptr;
        entry.interp_only = true;
        entry.interp_only_count = instr_count;
        entry.ends_with_branch = ir_block.ends_with_branch;
        entry.chain_target_pc = 0;
        entry.chained = false;
        entry.instr_count = instr_count;
        entry.verified_once = true;  // skip verify for interp-only
        blocks_[start_pc] = entry;
        blocks_translated++;
        block_profile.interp_only++;
        make_executable();  // W^X: balance the make_writable() at entry
        return nullptr;
    }
    // mode (which re-runs the interpreter for instr_count steps). With
    // BL_CALL, the JIT block executes MORE ARM instructions than
    // instr_count (the called functions run inside jit_call_helper).
    // The interpreter re-run would only do instr_count steps, missing
    // the called functions, causing false divergences. Mark as
    // verified_once to skip verify for BL_CALL blocks.
    // IR ops (not just BL instructions). When BL_CALL is disabled,
    // BL ends the block (like before), so verify is safe.
    // CALL_INTERP blocks can't be verified because the JIT flushes vregs
    // to cpu.regs[] before calling the interpreter, but the verify mode
    // restores the pre-JIT state for the interpreter re-run. This causes
    // false-positive divergences that compound and break programs like
    // curl. The JIT's output is correct — the verify mode's re-run is wrong.
    bool has_bl_call = false;
    bool has_call_interp = false;
    for (auto& ir_inst : ir_block.insts) {
        if (ir_inst.op == IROp::BL_CALL) { has_bl_call = true; break; }
        if (ir_inst.op == IROp::CALL_INTERP) { has_call_interp = true; }
    }
    // ── Optimize the IR ──────────────────────────────────────────
    static bool no_opt_ = (getenv("BIFROST_NO_OPT") != nullptr);
    if (!no_opt_) optimize_ir(ir_block);
    static bool dump_ir_ = (getenv("BIFROST_JIT_DUMP") != nullptr);
    if (dump_ir_) {
        fprintf(stderr, "══ Block @ 0x%llx (%d ARM instrs) ══\n",
                static_cast<unsigned long long>(start_pc), instr_count);
        dump_ir(ir_block);
    }
    // ── Compute stack size and pre-allocate vreg slots ────────────
    // Pre-scan IR to find all scratch vregs (33+) and assign each a
    // fixed stack slot. This avoids the lazy allocation mismatch between
    // the pre-computed stack size and the runtime slot counter.
    //
    // the previous code limited max_vreg to < 200,
    // but blocks with many ARM instructions (e.g., __multf3's 82-instr
    // block) can have vregs up to 317+. Vregs >= 200 would get stack
    // slots via lazy allocation that extend BEYOND the pre-allocated
    // stack frame, causing stack corruption.
    int max_vreg = 33;
    for (auto& inst : ir_block.insts) {
        if (inst.dest > max_vreg) max_vreg = inst.dest;
        if (inst.src1 > max_vreg) max_vreg = inst.src1;
        if (inst.src2 > max_vreg) max_vreg = inst.src2;
    }
    // Bounds-check: vreg arrays are fixed-size (4096). A pathological block
    // could exceed this, silently overflowing vreg_slot_[] / vreg_home_[] /
    // vreg_dirty_[]. Cap max_vreg and emit a warning to stderr the first
    // time this happens (the block will still run via CALL_INTERP fallback
    // for the overflowed vregs, which is correct but slow).
    static bool vreg_overflow_warned_ = false;
    if (max_vreg >= 4096) {
        if (!vreg_overflow_warned_) {
            fprintf(stderr, "[%s] frostJIT: vreg overflow (max=%d >= 4096) — "
                    "block will use interpreter fallback for overflowed vregs. "
                    "This is a bug; please report the guest binary.\n",
                    CODENAME, max_vreg);
            vreg_overflow_warned_ = true;
        }
        max_vreg = 4095;
    }
    // Pre-assign stack slots: vreg 33 → slot -8, vreg 34 → slot -16, etc.
    for (int v = 33; v <= max_vreg; v++) {
        vreg_slot_[v] = -8 * (v - 32);
    }
    num_stack_slots_ = max_vreg - 32;
    if (num_stack_slots_ < 1) num_stack_slots_ = 1;
    uint32_t stack_bytes = static_cast<uint32_t>(num_stack_slots_ * 8 + 64) & ~15U;
    // ── Vector register cache pre-scan ─────────────────────────────
    // Enable the XMM vector cache only if EVERY vector-touching op in the
    // block is cache-aware (SIMD_FP_FMA / SIMD_FP_ARITH) plus GPR-only ops.
    // The cache's correctness relies on never flushing mid-block, which the
    // all-or-nothing scan guarantees.
    vec_cache_may_enable(ir_block);
    // ── Prologue ─────────────────────────────────────────────────
    emit_push(RBX); emit_push(RBP); emit_push(R12);
    emit_push(R13); emit_push(R14); emit_push(R15);
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xE5); // mov rbp, rsp
    emit_byte(0x48); emit_byte(0x81); emit_byte(0xEC);
    emit_u32(stack_bytes);  // sub rsp, stack_bytes
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xFB); // mov rbx, rdi
    emit_byte(0x49); emit_byte(0x89); emit_byte(0xF6); // mov r14, rsi
    // v1.5.2-alpha: load the direct-window base into R10 ONLY if the
    // block actually touches guest memory through the direct window.
    // Previously every block paid a 10-byte movabs r10, imm64 in its
    // prologue — pure overhead for the many tiny ALU/FP/vector blocks
    // that never load or store guest memory (avg 3.5 guest instrs/block,
    // ~20M block entries/sec). The window-using IR ops are exactly:
    //   LOAD_MEM / STORE_MEM            (emit_load_mem / emit_store_mem)
    //   ATOMIC                          (direct-window lock fast path)
    //   SIMD_LD16 / SIMD_ST16           (16-byte vector memory access)
    // Everything else either never reads guest memory or calls a C
    // helper (which uses the pages_ table, not R10).
    {
        bool uses_window = false;
        for (const auto& inst : ir_block.insts) {
            switch (inst.op) {
                case IROp::LOAD_MEM:
                case IROp::STORE_MEM:
                case IROp::ATOMIC:
                case IROp::SIMD_LD16:
                case IROp::SIMD_ST16:
                    uses_window = true;
                    break;
                default:
                    break;
            }
            if (uses_window) break;
        }
        if (window_base_ && uses_window)
            emit_mov_imm64(WIN_REG, reinterpret_cast<uint64_t>(window_base_));
    }
    // Vector cache prologue loads MUST be emitted before block_body_start_off_
    // is recorded: self-loop re-entry jumps directly to the body start, so
    // these loads run only on cold entry (dispatcher / chain entry) and the
    // pinned XMM regs persist as loop-carried state across iterations.
    vec_emit_prologue_loads();
    // Record the block body start offset (after prologue). Used for
    // self-loop chaining: the selfloop slot is patched to jmp here.
    block_body_start_off_ = code_buf_used_;
    // ── Compute liveness: for each vreg, find the last op that uses it ──
    // This lets us free host regs of dead vregs immediately after their
    // last use, instead of keeping them cached until eviction. Without
    // this, the register allocator treats all vregs as live until the end
    // of the block, causing unnecessary spills when register pressure is
    // high (e.g., 5+ live vregs but 9 host regs, with 4+ dead vregs
    // occupying the other regs).
    //
    // Only scratch vregs (33+) are tracked for killing. ARM reg vregs
    // (0-31) represent architectural state that must be flushed to
    // cpu.regs[] at the epilogue — killing a dirty ARM reg vreg early
    // would lose its value before flush_all_vregs can write it back.
    //
    // Special cases for the use-scan:
    //   - LOAD_REG: src1 is the ARM reg index (0-31), NOT a vreg. Don't
    //     treat it as a use.
    //   - STORE_REG: dest is the ARM reg index, src1 is the vreg being
    //     stored. src1 IS a use.
    //   - dest of any op is a definition (not a use). If dest == src1
    //     (in-place modify), the vreg is still live after this op (as
    //     dest), so we don't kill it (handled in the compile loop below).
    {
        size_t n = ir_block.insts.size();
        std::vector<int> last_use(4096, -1);
        for (size_t i = 0; i < n; i++) {
            const IRInst& inst = ir_block.insts[i];
            if (inst.op != IROp::LOAD_REG) {
                // src1/src2 are vregs (for LOAD_REG, src1 is ARM reg index).
                // Bounds-check: vreg space is 0-4095. A block with vregs
                // >=4096 indicates a translator bug; cap to avoid OOB.
                if (inst.src1 > 31 && inst.src1 < 4096)
                    last_use[inst.src1] = static_cast<int>(i);
                if (inst.src2 > 31 && inst.src2 < 4096)
                    last_use[inst.src2] = static_cast<int>(i);
            }
        }
        // Build kills_per_op_: for each op i, the list of scratch vregs
        // whose last use is i (dest is excluded at kill time in the loop).
        kills_per_op_.assign(n, {});
        for (int v = 32; v < 4096; v++) {
            if (last_use[v] >= 0) {
                kills_per_op_[last_use[v]].push_back(static_cast<uint16_t>(v));
            }
        }
    }
    // ── Compile IR ───────────────────────────────────────────────
    for (size_t i = 0; i < ir_block.insts.size(); i++) {
        const IRInst& inst = ir_block.insts[i];
        if (compile_ir_inst(inst)) break;
        // Free host regs of vregs whose last use was this op. Dead vregs
        // are freed immediately, making room for new vregs without eviction.
        if (i < kills_per_op_.size()) {
            for (uint16_t v : kills_per_op_[i]) {
                // Don't kill the dest of this op — it's a new definition
                // and still live (handles dest == src1 in-place modify).
                if (v != inst.dest) {
                    kill_vreg(v);
                }
            }
        }
    }
    kills_per_op_.clear();
    // ── Epilogue ─────────────────────────────────────────────────
    size_t epilogue_off = code_buf_used_;
    // Materialize pending host flags to cpu.pstate before returning.
    // If a flag-setting op (ADDS/SUBS/TST) was the last to touch flags
    // and no subsequent BRCOND consumed them, the flags are still in
    // the host CPU's RFLAGS but haven't been written to pstate. The
    // next block (or the interpreter) would see stale pstate.
    // clobber_flags() handles the flush+materialize+invalidate pattern.
    clobber_flags();
    // Flush all dirty vregs before returning (so cpu.regs[] is up to date).
    flush_all_vregs();
    // Write back dirty cached vectors to cpu.v_lo/v_hi. Self-loop re-entry
    // skips the epilogue, so loop-carried accumulators stay in XMM; this
    // runs only when the loop exits (or the block is entered from another
    // block / the dispatcher).
    vec_cache_writeback_all();
    if (!rax_holds_next_pc_) {
        uint64_t next_pc = start_pc + ir_block.count * 4;
        emit_mov_imm_to_rax(next_pc);
        // Fall-through (MAX_BLOCK hit before any block-ender): the next
        // PC is statically known, so this block is chainable to it.
        if (!unchainable_end_ && chain_target_pc_ == 0) {
            chain_target_pc_ = next_pc;
        }
    }
    // ── PC store point ───────────────────────────────────────────
    // Both the normal epilogue and the CALL_INTERP early-exit converge
    // here. At this point, flags are materialized and vregs are flushed.
    // RAX holds the next PC (either from rax_holds_next_pc_ or from the
    // CALL_INTERP's interpreter call).
    // call_interp_branch_patches_ jump here (after the
    // RAX overwrite) to preserve the interpreter's PC in RAX.
    size_t pc_store_off = code_buf_used_;
    emit_store(CPU_REG, PC_OFF, RAX);
    // ── Chain-capable epilogue ────────────────────────────────────
    // For block chaining we jump directly from one block's epilogue to
    // the next block's prologue. The next prologue reloads RBX/R14 from
    // RDI/RSI (the System-V arg registers), so before restoring our own
    // callee-saved regs we copy the live CPU/EMU pointers into RDI/RSI.
    // When the block returns (unchained `ret`), clobbering RDI/RSI is
    // fine — they're caller-saved and the dispatcher doesn't read them
    // after the call. When the block is chained, the next prologue sees
    // the correct RDI=cpu / RSI=emu.
    emit_mov_reg(RDI, CPU_REG);   // mov rdi, rbx
    emit_mov_reg(RSI, EMU_REG);   // mov rsi, r14
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp, rbp
    emit_pop(R15); emit_pop(R14); emit_pop(R13);
    emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
    // ── Chain slot ──
    // 5 bytes reserved at the end of every block. Initially `ret` + 4
    // NOPs (acts as a plain return to the C dispatcher). When the
    // block's chain target has been translated, patch_chain() overwrites
    // all 5 bytes with `jmp rel32` → target block's entry, skipping the
    // dispatcher entirely for straight-line / unconditional-branch code.
    size_t chain_patch_off = code_buf_used_;
    emit_ret();                                  // 0xC3
    emit_nop(); emit_nop(); emit_nop(); emit_nop();  // 4 × 0x90
    // Patch branch targets to epilogue.
    for (auto& p : branch_target_patches_) {
        int32_t rel = static_cast<int32_t>(epilogue_off - (p.patch_off + 5));
        patch_jmp_rel32(p.patch_off, rel);
    }
    for (size_t off : call_interp_branch_patches_) {
        // jump to pc_store_off (after the RAX overwrite)
        // to preserve the interpreter's PC in RAX. emit_call_interp already
        // materialized flags and flushed vregs before the JNE, so we can
        // skip the normal epilogue's flag/vreg handling.
        int32_t rel = static_cast<int32_t>(pc_store_off - (off + 6));
        patch_jcc_rel32(off, rel);
    }
    if (code_buf_overflow_) {
        code_buf_used_ = block_start;
        // W^X: make the buffer executable again before returning (we may
        // have written partial code before the overflow was detected).
        make_executable();
        return nullptr;
    }
    if (dump_ir_) {
        size_t code_len = code_buf_used_ - block_start;
        fprintf(stderr, "  → %zu bytes of x86 code @ %p:\n    ",
                code_len, static_cast<void*>(code_buf_ + block_start));
        for (size_t i = 0; i < code_len; i++) {
            fprintf(stderr, "%02x ", code_buf_[block_start + i]);
            if ((i & 31) == 31 && i + 1 < code_len) fprintf(stderr, "\n    ");
        }
        fprintf(stderr, "\n");
    }
    auto fn = (uint64_t(*)(CPU*, Emulator*))(code_buf_ + block_start);
    // If the block ended with an unchainable op (BR/BRCOND/SVC), force
    // chain_target_pc_ to 0 so try_chain_block() skips it.
    if (unchainable_end_) chain_target_pc_ = 0;
    BlockEntry entry;
    entry.fn = fn;
    entry.ends_with_branch = ir_block.ends_with_branch;
    entry.chain_patch_off = chain_patch_off;
    entry.chain_target_pc = chain_target_pc_;
    entry.chained = false;
    // Taken-path chain info (BRCOND/CBZ/CBNZ/TBZ/TBNZ emit a second chain
    // slot on the taken path; see emit_taken_path_epilogue).
    entry.has_taken_chain_slot = has_taken_chain_slot_;
    entry.taken_chain_patch_off = taken_chain_patch_off_;
    entry.taken_chain_target_pc = taken_chain_target_pc_;
    entry.taken_chained = false;
    entry.instr_count = instr_count;
    entry.call_interp_count = call_interp_count;
    entry.verified_once = has_bl_call || has_call_interp;
    // Record self-loop info: if the block has a selfloop slot, patch it
    // to jump back to the block body start (skipping epilogue+dispatcher+
    // prologue). This is the single biggest win for tight loops.
    if (has_selfloop_slot_) {
        entry.has_selfloop_slot = true;
        entry.selfloop_patch_off = selfloop_patch_off_;
        // Patch the selfloop slot: `jmp rel32` → block body start.
        size_t body_off = block_body_start_off_;
        int32_t self_rel = static_cast<int32_t>(body_off - (selfloop_patch_off_ + 5));
        code_buf_[selfloop_patch_off_] = 0xE9;  // jmp rel32
        code_buf_[selfloop_patch_off_ + 1] = self_rel & 0xFF;
        code_buf_[selfloop_patch_off_ + 2] = (self_rel >> 8) & 0xFF;
        code_buf_[selfloop_patch_off_ + 3] = (self_rel >> 16) & 0xFF;
        code_buf_[selfloop_patch_off_ + 4] = (self_rel >> 24) & 0xFF;
    }
    // ── Verify-mode memory save/restore: populate store_infos ──────
    // Walk the IR and record every STORE_MEM whose address operand can
    // be statically resolved to (saved_arm_reg + offset). At verify
    // time, we use this list to snapshot the original memory values
    // before the JIT runs and restore them before the interpreter
    // re-runs the block. This eliminates false-positive divergences
    // caused by the JIT's STORE_MEM being visible to the interpreter's
    // LOAD_MEM (read-then-write-same-address pattern).
    //
    // Address tracing rules (forward dataflow over the IR):
    //   LOAD_REG v ← arm_reg:           vreg_base[v] = arm_reg, vreg_off[v] = 0
    //   IMM      v ← imm:               vreg_base[v] = 0xFF, vreg_off[v] = imm
    //   ADD      v = v1 + v2 (one IMM): vreg_base[v] = vreg_base[v1], vreg_off[v] = vreg_off[v1] + imm
    //   anything else:                  vreg_base[v] = 0xFF (unknown)
    // STORE_MEM is recordable iff vreg_base[src1] is a valid ARM reg AND
    // that ARM reg is NOT written by any STORE_REG in this block (otherwise
    // the address would use the post-modification value, not the saved one).
    {
        // Forward dataflow: vreg → (base_arm_reg, accumulated_offset).
        // base_arm_reg = 0xFF means "unknown" (not a simple base+off).
        // We ALSO track which ARM regs have been modified SO FAR (up to
        // the current instruction). A STORE_MEM is only recordable if its
        // base ARM reg has NOT been modified before the STORE_MEM. This
        // handles the common pattern where a block loads from [Xn, #imm],
        // stores to [Xn, #imm], THEN does writeback (Xn = Xn + imm). The
        // store uses the ORIGINAL Xn value, which matches the saved CPU
        // state — so we can safely save/restore. The previous whole-block
        // check was too conservative and skipped these stores, leaving
        // false-positive divergences (e.g. fcvtzu_test's block at 0x1740
        // where LDR x2,[x1] ... STR x0,[x1] ... ADD x1,x1,#4).
        std::vector<uint8_t> vreg_base(4096, 0xFF);
        std::vector<int64_t> vreg_off(4096, 0);
        uint32_t modified_so_far = 0;  // bit i set if ARM reg i has been STORE_REG'd SO FAR
        // When a STORE_MEM's base reg was modified, we can still record it if we know
        // the new absolute value. This fixes the curl URL parse divergence where
        // x19 was set to a high address (0x571c67a000) via IMM, then used as a
        // STORE_MEM base. The old code skipped this store, causing the interpreter
        // to see the JIT's modification — a false-positive PC divergence.
        std::vector<bool> arm_reg_known(32, false);
        std::vector<uint64_t> arm_reg_val(32, 0);
        for (auto& inst : ir_block.insts) {
            if (inst.op == IROp::LOAD_REG && inst.src1 <= 31) {
                if (inst.dest < 4096) {
                    vreg_base[inst.dest] = static_cast<uint8_t>(inst.src1);
                    vreg_off[inst.dest]  = 0;
                }
            } else if (inst.op == IROp::IMM) {
                if (inst.dest < 4096) {
                    vreg_base[inst.dest] = 0xFE;  // marker: this is an IMM
                    vreg_off[inst.dest]  = static_cast<int64_t>(inst.imm);
                }
            } else if (inst.op == IROp::ADD) {
                // ADD v, v1, v2 — try to fold if one operand is IMM
                uint8_t b1 = (inst.src1 < 4096) ? vreg_base[inst.src1] : 0xFF;
                uint8_t b2 = (inst.src2 < 4096) ? vreg_base[inst.src2] : 0xFF;
                if (b1 <= 31 && b2 == 0xFE && inst.dest < 4096) {
                    // base + imm
                    vreg_base[inst.dest] = b1;
                    vreg_off[inst.dest]  = vreg_off[inst.src1] + vreg_off[inst.src2];
                } else if (b2 <= 31 && b1 == 0xFE && inst.dest < 4096) {
                    // imm + base (commutative)
                    vreg_base[inst.dest] = b2;
                    vreg_off[inst.dest]  = vreg_off[inst.src1] + vreg_off[inst.src2];
                } else {
                    if (inst.dest < 4096) vreg_base[inst.dest] = 0xFF;
                }
            } else if (inst.op == IROp::STORE_MEM) {
                // Record if src1 traces back to an ARM reg.
                uint8_t b = (inst.src1 < 4096) ? vreg_base[inst.src1] : 0xFF;
                if (b <= 31) {
                    BlockEntry::StoreInfo si;
                    si.arm_reg = b;
                    si.offset  = vreg_off[inst.src1] + static_cast<int64_t>(inst.imm);
                    si.width   = inst.width;
                    si.use_absolute = false;
                    if (!(modified_so_far & (1u << b))) {
                        // Base reg NOT modified — use saved.regs[b] + offset (original path).
                        // Already set: si.use_absolute = false.
                    } else if (arm_reg_known[b]) {
                        // Compute the absolute address and use that for snapshot/restore.
                        si.use_absolute = true;
                        si.absolute_addr = arm_reg_val[b] + static_cast<uint64_t>(si.offset);
                    } else {
                        // Base reg was modified but we don't know the new value.
                        // Skip (accept false positive).
                        continue;
                    }
                    if (!entry.store_infos) {
                        entry.store_infos = std::make_shared<std::vector<BlockEntry::StoreInfo>>();
                    }
                    entry.store_infos->push_back(si);
                }
            } else if (inst.op == IROp::STORE_REG) {
                // Mark the dest ARM reg as modified FROM THIS POINT ON.
                if (inst.dest <= 31) {
                    modified_so_far |= (1u << inst.dest);
                    // new absolute value of this ARM reg.
                    uint8_t src_base = (inst.src1 < 4096) ? vreg_base[inst.src1] : 0xFF;
                    if (src_base == 0xFE) {
                        arm_reg_known[inst.dest] = true;
                        arm_reg_val[inst.dest] = static_cast<uint64_t>(vreg_off[inst.src1]);
                    } else {
                        arm_reg_known[inst.dest] = false;
                    }
                }
            } else {
                // Any other op destroys the base-tracking property for dest.
                if (inst.dest < 4096 && inst.op != IROp::STORE_REG) {
                    vreg_base[inst.dest] = 0xFF;
                }
            }
        }
    }
    blocks_[start_pc] = entry;
    blocks_translated++;
    // Maintain the back-reference index: this block at start_pc has
    // chain_target_pc=T, so add start_pc to back_refs_[T]. This lets
    // chain_back_references(T) find this block in O(k) instead of
    // scanning all blocks.
    //
    // Cap: if back_refs_ grows too large (rare — only happens for
    // very long-running programs with millions of unique PCs), clear
    // it and let it rebuild lazily. This prevents unbounded memory
    // growth. The cap is generous (1M entries ≈ 50MB) so it never
    // fires in practice.
    if (chain_target_pc_ != 0) {
        if (back_refs_.size() > 1000000) {
            back_refs_.clear();
        }
        back_refs_[chain_target_pc_].push_back(start_pc);
    }
    // The taken-path edge is chained too: add start_pc to its target's
    // back-ref list so chain_back_references(T) can patch the taken slot
    // when T gets translated later (e.g. a loop whose back-edge target is
    // only translated on the second iteration).
    if (taken_chain_target_pc_ != 0 && taken_chain_target_pc_ != chain_target_pc_) {
        if (back_refs_.size() > 1000000) {
            back_refs_.clear();
        }
        back_refs_[taken_chain_target_pc_].push_back(start_pc);
    }
    // Try to chain this block to its already-translated target, and
    // also patch any existing blocks whose chain target is this block.
    // BIFROST_NO_CHAIN disables chaining for debugging.
    static bool no_chain_ = (getenv("BIFROST_NO_CHAIN") != nullptr);
    if (!no_chain_) {
        try_chain_block(start_pc, blocks_[start_pc]);
        chain_back_references(start_pc);
    }
    // Save max_vreg_ so the next translate_block only clears what's needed.
    prev_max_vreg_ = max_vreg_;
    // Verify dirty_host_regs_ invariant (active in debug or with
    // BIFROST_REGALLOC_CHECK=1 — catches regalloc maintenance bugs).
    verify_dirty_host_regs_();
    // W^X: toggle the code buffer back to executable before returning.
    // The block is fully emitted and patched; execution will read from it.
    make_executable();
    return fn;
}
} // namespace arm64emu
