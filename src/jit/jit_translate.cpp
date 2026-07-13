// jit/jit_translate.cpp — FrostJIT block translation.
//
// v1.4.5-alpha (Turn 36): split out of frostjit.cpp. Holds the
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
#include "ir/ir.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace arm64emu {

// Turn 91: defined in ir_translate.cpp. When true, BL uses old behavior
// (end block) instead of BL_CALL (call within block).
extern thread_local bool bl_call_disabled_;

// ── instr_will_call_interp — heuristic for block splitting ─────────────
// Returns true if the given ARM64 instruction is likely to generate a
// CALL_INTERP IR op (i.e. the JIT can't codegen it natively). Used by
// translate_block to decide where to split blocks — too many
// CALL_INTERP fallbacks in one block causes register pressure issues.
//
// v1.4.5-alpha (Turn 36): moved here from jit_flags.cpp (where it was
// extracted by accident — it's only used by translate_block).
static bool instr_will_call_interp(const DecodedInst& d) {
    switch (d.cls) {
        case InstClass::SIMD_DP: {
            uint32_t op = d.raw;
            uint8_t size = (op >> 22) & 3;
            uint32_t sub3 = op & 0xFF20FC00;
            uint32_t sub3_noq = sub3 & ~(1u << 30);
            if (sub3_noq == 0x0E208400 ||  // ADD
                sub3_noq == 0x2E208400 ||  // SUB
                (sub3_noq == 0x0E209C00 && size != 3) ||  // MUL (not 64-bit)
                sub3_noq == 0x2E208C00) {  // CMEQ
                return false;  // native SIMD_ARITH / SIMD_CMP
            }
            return true;  // other SIMD_DP ops fall back to interp
        }
        case InstClass::FP_SCALAR:
        case InstClass::LDXR: case InstClass::STXR:
        case InstClass::LDAXR: case InstClass::STLXR:
        case InstClass::LDAR: case InstClass::STLR:
            return true;
        default:
            break;
    }
    if (d.is_vec &&
        (d.cls == InstClass::LDR_IMM || d.cls == InstClass::LDR_UNS ||
         d.cls == InstClass::LDR_REG || d.cls == InstClass::STR_IMM ||
         d.cls == InstClass::STR_UNS || d.cls == InstClass::STR_REG ||
         d.cls == InstClass::LDP || d.cls == InstClass::STP)) {
        return true;
    }
    return false;
}

// ── translate_block ───────────────────────────────────────────────
uint64_t (*FrostJIT::translate_block(Emulator& emu, uint64_t start_pc))(CPU*, Emulator*) {
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
    num_stack_slots_ = 0;
    // Only clear the vreg arrays up to the previous block's max_vreg_+1,
    // not all 4096 entries. This saves ~12KB of writes per block
    // translation when blocks are small (typical: max_vreg_ ≈ 33-100).
    int clear_limit = prev_max_vreg_ + 1;
    if (clear_limit > 4096) clear_limit = 4096;
    for (int i = 0; i < clear_limit; i++) {
        vreg_home_[i] = -1;
        vreg_dirty_[i] = false;
        vreg_slot_[i] = 0;
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
    // Turn 92: limit BL_CALL per block. Each BL_CALL flushes all vregs and
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
            chain_target_pc_ = cur_pc;
            break;
        }
        uint32_t inst;
        try {
            inst = emu.mem().fetch_inst(cur_pc);
        } catch (...) { break; }
        DecodedInst d;
        if (!decode(d, inst)) break;

        bool will_call_interp = instr_will_call_interp(d);

        if (will_call_interp && call_interp_count >= MAX_CALL_INTERP_PER_BLOCK && instr_count > 0) {
            // Split here — the next instruction starts a new block.
            chain_target_pc_ = cur_pc;
            break;
        }

        bool ends = translate_to_ir(ir_block, d, cur_pc);
        if (will_call_interp) call_interp_count++;
        // Track BL_CALL count for block splitting.
        if (d.cls == InstClass::BL) bl_call_count++;
        instr_count++;
        ir_block.count = instr_count;
        if (ends) block_ended = true;
        else {
            // Turn 92: split block after MAX_BL_CALL_PER_BLOCK BL_CALLs.
            if (bl_call_count >= MAX_BL_CALL_PER_BLOCK) {
                chain_target_pc_ = cur_pc + 4;
                break;
            }
            cur_pc += 4;
        }
    }
    if (instr_count == 0) {
        make_executable();  // W^X: balance the make_writable() at entry
        return nullptr;
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
        make_executable();  // W^X: balance the make_writable() at entry
        return nullptr;
    }

    // Turn 92: BL_CALL blocks can't be verified by the current verify
    // mode (which re-runs the interpreter for instr_count steps). With
    // BL_CALL, the JIT block executes MORE ARM instructions than
    // instr_count (the called functions run inside jit_call_helper).
    // The interpreter re-run would only do instr_count steps, missing
    // the called functions, causing false divergences. Mark as
    // verified_once to skip verify for BL_CALL blocks.
    // Turn 96: only skip verify if the block ACTUALLY contains BL_CALL
    // IR ops (not just BL instructions). When BL_CALL is disabled,
    // BL ends the block (like before), so verify is safe.
    bool has_bl_call = false;
    for (auto& ir_inst : ir_block.insts) {
        if (ir_inst.op == IROp::BL_CALL) { has_bl_call = true; break; }
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

    // ── Prologue ─────────────────────────────────────────────────
    emit_push(RBX); emit_push(RBP); emit_push(R12);
    emit_push(R13); emit_push(R14); emit_push(R15);
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xE5); // mov rbp, rsp
    emit_byte(0x48); emit_byte(0x81); emit_byte(0xEC);
    emit_u32(stack_bytes);  // sub rsp, stack_bytes

    emit_byte(0x48); emit_byte(0x89); emit_byte(0xFB); // mov rbx, rdi
    emit_byte(0x49); emit_byte(0x89); emit_byte(0xF6); // mov r14, rsi
    if (window_base_) emit_mov_imm64(WIN_REG, reinterpret_cast<uint64_t>(window_base_));

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
    entry.instr_count = instr_count;
    entry.call_interp_count = call_interp_count;
    // Turn 91: BL_CALL blocks can't be verified (see comment above).
    entry.verified_once = has_bl_call;
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
                // Record if src1 traces back to an ARM reg that hasn't been
                // modified SO FAR (before this STORE_MEM).
                uint8_t b = (inst.src1 < 4096) ? vreg_base[inst.src1] : 0xFF;
                if (b <= 31 && !(modified_so_far & (1u << b))) {
                    BlockEntry::StoreInfo si;
                    si.arm_reg = b;
                    si.offset  = vreg_off[inst.src1] + static_cast<int64_t>(inst.imm);
                    si.width   = inst.width;
                    // Lazily create the vector on first store (shared_ptr
                    // so hot-path BlockEntry copy is cheap — atomic
                    // refcount++ instead of vector deep-copy).
                    if (!entry.store_infos) {
                        entry.store_infos = std::make_shared<std::vector<BlockEntry::StoreInfo>>();
                    }
                    entry.store_infos->push_back(si);
                }
            } else if (inst.op == IROp::STORE_REG) {
                // Mark the dest ARM reg as modified FROM THIS POINT ON.
                if (inst.dest <= 31) {
                    modified_so_far |= (1u << inst.dest);
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
