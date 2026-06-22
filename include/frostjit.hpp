// frostjit.hpp — IR → x86-64 JIT for bifrost-emu (v1.4.0-alpha.3)
//
// ── Architecture ──────────────────────────────────────────────────────
//
//   ARM64 block → translate_to_ir() → IRBlock
//                → optimize_ir()     → smaller IRBlock
//                → compile_block()   → native x86-64 code in code_buf_
//
// The compiled block is a function:
//
//     uint64_t block_fn(CPU* cpu /*RDI*/, Emulator* emu /*RSI*/);
//
// Returns the next guest PC.
//
// ── Register usage (persistent across block) ──────────────────────────
//   RBX = CPU*                (callee-saved, set in prologue)
//   R14 = Emulator*           (callee-saved, set in prologue)
//   R10 = direct_window base  (callee-saved, set in prologue)
//
// ── Scratch (per-instruction) ─────────────────────────────────────────
//   RAX, RCX, RDX, R8, R9, R11
//
// ── ARM64 reg layout in CPU struct ────────────────────────────────────
//   regs[0..30] at offset 0..247  (8 bytes each)
//   sp          at offset 248
//   pc          at offset 256
//   pstate      at offset 264
//
// ── Inline interpreter fallback ───────────────────────────────────────
// For unsupported IR ops (CALL_INTERP for ARM64 instructions we don't
// model in IR), we spill all dirty vregs, set cpu.pc, call
// emu->step_public(cpu), reload vregs, and check if PC changed. The
// block does NOT split — the interpreter call is inline. This is the
// "inline interpreter fallback" pattern that avoids block-splitting
// overhead.
#pragma once

#include "decoder.hpp"
#include "ir.hpp"
#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace arm64emu {

struct CPU;
class Emulator;
class Memory;

// Interpreter step function (called inline by JIT for unsupported ops).
extern "C" void jit_interp_step(Emulator* emu, CPU* cpu);

class FrostJIT {
public:
    FrostJIT();
    ~FrostJIT();

    FrostJIT(const FrostJIT&) = delete;
    FrostJIT& operator=(const FrostJIT&) = delete;

    void set_direct_window(uint8_t* base) { window_base_ = base; }
    uint64_t run_block(CPU& cpu, Emulator& emu);

    uint64_t blocks_translated = 0;
    uint64_t blocks_executed   = 0;
    uint64_t cache_hits        = 0;
    uint64_t cache_misses      = 0;
    uint64_t interpreter_fallbacks = 0;
    uint64_t block_chains_patched = 0;

    // Loop watchdog state — per-instance so multiple FrostJIT objects
    // (e.g. one per thread) don't share/corrupt each other's counters.
    // Resets on any different PC; if the same PC runs > 100K times in
    // a row, fall back to the interpreter to break the loop.
    static constexpr uint32_t WATCHDOG_LIMIT = 100000;
    uint64_t watchdog_last_pc_ = UINT64_MAX;
    uint32_t watchdog_count_   = 0;

    void flush_cache();
    size_t code_buf_used()  const { return code_buf_used_; }
    size_t code_buf_size()  const { return 64 * 1024 * 1024; }
    size_t cache_entries()  const { return blocks_.size(); }
    const uint8_t* code_buf() const { return code_buf_; }

    static constexpr int REGS_OFF   = 0;
    static constexpr int SP_OFF     = 256;
    static constexpr int PC_OFF     = 264;
    static constexpr int PSTATE_OFF = 272;
    static constexpr int V_LO_OFF   = 288;  // v_lo[0] — 32 × uint64_t
    static constexpr int V_HI_OFF   = 544;  // v_hi[0] — 32 × uint64_t
    static constexpr int FPCR_OFF   = 800;
    static constexpr int FPSR_OFF   = 804;

    // x86 reg constants.
    static constexpr int RAX=0, RCX=1, RDX=2, RBX=3, RSP=4;
    static constexpr int RBP=5, RSI=6, RDI=7;
    static constexpr int R8=8, R9=9, R10=10, R11=11;
    static constexpr int R12=12, R13=13, R14=14, R15=15;
    static constexpr int CPU_REG = RBX;
    static constexpr int EMU_REG = R14;
    static constexpr int WIN_REG = R10;

private:
    static constexpr size_t CODE_BUF_SIZE = 64 * 1024 * 1024;
    uint8_t* code_buf_ = nullptr;
    size_t   code_buf_used_ = 0;
    bool     code_buf_overflow_ = false;
    uint8_t* window_base_ = nullptr;

    // ── Block chaining ──────────────────────────────────────────────
    // Each block ends with a 5-byte "chain slot" that is initially
    // `ret` + 4 NOPs. When the block's statically-known next PC (its
    // "chain target") has been translated, the slot is patched in
    // place to `jmp rel32` → next block's entry. This lets straight-
    // line code skip the C dispatcher entirely between linked blocks.
    //
    // chain_target_pc_ == 0 means "not chainable" (indirect branch,
    // SVC, conditional branch — runtime-dependent next PC).
    struct BlockEntry {
        uint64_t (*fn)(CPU*, Emulator*) = nullptr;
        bool ends_with_branch = false;
        size_t  chain_patch_off = 0;   // offset of the 5-byte chain slot in code_buf_
        uint64_t chain_target_pc = 0;  // statically-known next PC, or 0
        bool    chained = false;       // true once the slot has been patched to a jmp
        int     instr_count = 0;      // number of ARM64 instructions in this block
        size_t  body_off = 0;         // offset of the IR body (after prologue) — for frameless back-edge chaining
        bool    frameless_compatible = false; // true if the block can be the target of a frameless back-edge jump
    };
    std::unordered_map<uint64_t, BlockEntry> blocks_;

    // Back-reference index: maps target_pc → list of source_pcs whose
    // chain_target_pc equals target_pc. Maintained incrementally at
    // translate-time (each block adds itself to its target's back-ref
    // list). Lets chain_back_references run in O(k) where k is the
    // number of back-refs (typically 1-3), instead of O(N) scanning
    // all blocks. This makes it cheap enough to call on every cache
    // hit, not just at translate-time.
    std::unordered_map<uint64_t, std::vector<uint64_t>> back_refs_;

    // Patch a block's chain slot to jump directly to `target_fn`.
    // Returns true if the patch was applied.
    bool patch_chain(size_t chain_patch_off, const uint8_t* target_fn);
    // Try to chain `entry` to its already-translated target (if any),
    // and try to chain any existing blocks whose target is `pc`.
    void try_chain_block(uint64_t pc, BlockEntry& entry);
    // Scan all cached blocks and chain any whose target is `target_pc`.
    // Uses back_refs_ for O(k) lookup; falls back to O(N) scan only if
    // the index is missing (defensive — should never happen).
    void chain_back_references(uint64_t target_pc);

    // ── x86 emitters ────────────────────────────────────────────────
    void emit_byte(uint8_t b);
    void emit_u32(uint32_t v);
    void emit_u64(uint64_t v);
    static uint8_t rex(bool w, bool r, bool x, bool b);
    static uint8_t modrm(uint8_t mod, uint8_t reg, uint8_t rm);
    static uint8_t sib(uint8_t scale, uint8_t index, uint8_t base);

    void emit_mov_imm64(int dst, uint64_t imm);
    void emit_mov_imm32(int dst, uint32_t imm);
    void emit_mov_imm32_zext(int dst, uint32_t imm);
    void emit_mov_reg(int dst, int src);
    void emit_load(int dst, int base, int32_t off);
    void emit_store(int base, int32_t off, int src);
    void emit_load32(int dst, int base, int32_t off);
    void emit_store32(int base, int32_t off, int src);
    void emit_load16(int dst, int base, int32_t off);
    void emit_load8(int dst, int base, int32_t off);
    void emit_load32_sx(int dst, int base, int32_t off);
    void emit_load16_sx(int dst, int base, int32_t off);
    void emit_load8_sx(int dst, int base, int32_t off);
    void emit_store16(int base, int32_t off, int src);
    void emit_store8(int base, int32_t off, int src);
    void emit_modrm_disp(int reg, int base, int32_t off);

    void emit_add_reg(int dst, int src);
    void emit_sub_reg(int dst, int src);
    void emit_adc_reg(int dst, int src);   // adc r64, r64 (with CF)
    void emit_sbb_reg(int dst, int src);   // sbb r64, r64 (with CF)
    void emit_and_reg(int dst, int src);
    void emit_or_reg(int dst, int src);
    void emit_xor_reg(int dst, int src);
    void emit_imul_reg(int dst, int src);
    void emit_test_reg(int a, int b);
    void emit_cmp_reg(int a, int b);
    void emit_shift_cl(int dst, int kind);
    void emit_shift_imm8(int dst, int kind, uint8_t cnt);
    void emit_not_reg(int dst);
    void emit_neg_reg(int dst);
    void emit_bsf_reg(int dst, int src);
    void emit_bsr_reg(int dst, int src);
    void emit_lzcnt_reg(int dst, int src);
    void emit_popcnt_reg(int dst, int src);
    void emit_bswap_reg(int dst);
    void emit_setcc(int dst, uint8_t cc);
    void emit_cmovcc(int dst, int src, uint8_t cc);
    void emit_call_abs(void* target);
    void emit_ret();
    void emit_nop();
    void emit_push(int reg);
    void emit_pop(int reg);
    // pushfq / popfq — save/restore x86 RFLAGS to/from stack.
    // Replaces the magic byte sequences `emit_byte(0x9C)` / `emit_byte(0x9D)`
    // that were scattered across ~20 call sites.
    void emit_pushfq();
    void emit_popfq();

    // emit_call_aligned: emit a properly RSP-16-aligned call sequence.
    //
    // Background: the SysV AMD64 ABI requires RSP%16==0 at the point of
    // the CALL instruction (so that inside the callee, after the return
    // address is pushed, RSP%16==8 — and the callee's prologue typically
    // pushes RBP to restore alignment). At JIT body entry RSP%16==8
    // (prologue does 6 pushes + aligned sub). After the caller pushes
    // `num_pushed` additional regs to save them across the call, RSP%16
    // = (8 + 8*num_pushed) % 16. Adding `pushfq` (+1 push) makes the
    // total even iff `num_pushed` is even, yielding RSP%16==0.
    //
    // Rule:
    //   num_pushed EVEN → pushfq alone aligns (no sub rsp needed)
    //   num_pushed ODD  → sub rsp,8 first, then pushfq aligns
    //
    // The helper emits (in order):
    //   [sub rsp, 8]    (only if num_pushed is odd)
    //   pushfq
    //   call <target>
    //   popfq
    //   [add rsp, 8]    (only if num_pushed is odd)
    //
    // Caller pattern:
    //   emit_push(R10);                // save caller-saved
    //   emit_push(RAX);
    //   // ... set up RDI/RSI/RDX/RCX call args ...
    //   emit_call_aligned(&jit_foo, /*num_pushed=*/2);
    //   emit_pop(RAX);                 // restore in reverse order
    //   emit_pop(R10);
    //
    // The caller owns the push/pop of saved regs; the helper owns the
    // alignment fixup + flag save + the call itself.
    void emit_call_aligned(void* target, int num_pushed);

    // Function-pointer overload — see emit_call_abs template above.
    template <typename R, typename... Args>
    void emit_call_aligned(R (*fn)(Args...), int num_pushed) {
        emit_call_aligned(reinterpret_cast<void*>(fn), num_pushed);
    }
    size_t emit_jmp_rel32_placeholder();
    void patch_jmp_rel32(size_t off, int32_t rel);
    size_t emit_jcc_rel32_placeholder(uint8_t cc);
    void patch_jcc_rel32(size_t off, int32_t rel);
    // rel8 jumps (short, ±127 bytes). Return offset of placeholder; patch later.
    size_t emit_jcc_rel8_placeholder(uint8_t cc);
    void patch_jcc_rel8(size_t off, int8_t rel);
    size_t emit_jmp_rel8_placeholder();
    void patch_jmp_rel8(size_t off, int8_t rel);
    // Stack pointer adjustment (sub/add rsp, imm8).
    void emit_sub_rsp_imm8(uint8_t n);
    void emit_add_rsp_imm8(uint8_t n);

    // Mask CL register with an 8-bit immediate (`and cl, imm8`).
    // Used before variable shifts (shl/shr/sar/ror r, cl) to clamp
    // the shift count to the operand width. Replaces the magic-byte
    // sequence `emit_byte(0x48); emit_byte(0x83); emit_byte(0xE1); emit_byte(n);`.
    void emit_and_cl_imm8(uint8_t mask);

    // ARM64 reg access.
    void emit_load_arm(int xr, int ar);
    void emit_store_arm(int ar, int xr);

    // Flag materialization.
    void emit_materialize_flags(bool from_sub = false);
    void emit_load_flags_from_pstate();

    // Memory access (direct-window path).
    void emit_load_mem(int dst, int addr_reg, int32_t off, int w, bool sign_ext);
    void emit_store_mem(int addr_reg, int32_t off, int src_reg, int w);

    // Condition code mapping.
    uint8_t arm_cond_to_x86(uint8_t arm_cond) const;

    // ── Register allocator ─────────────────────────────────────────
    // Maps vregs to x86 registers. Each vreg has a "home" x86 reg (or -1
    // if spilled to stack). The allocator tracks which vreg owns each x86
    // reg. When a vreg is needed, its home reg is used directly (no
    // load/store). When a reg is needed for a new vreg, the old owner is
    // spilled if dirty.
    //
    // Scratch x86 regs available for allocation:
    //   Caller-saved (clobbered by C calls): RAX, RCX, RDX, R8, R9, R11
    //   Callee-saved (preserved by C calls): R12, R13, R15
    // Persistent: RBX=CPU, R14=EMU, R10=window, RBP=frame.
    //
    // (v1.4.0-alpha.5): added R12/R13/R15 (callee-saved) to the pool.
    // This gives 9 registers instead of 6, and vregs cached in
    // callee-saved regs survive CALL_INTERP without spilling — the C
    // calling convention preserves them across calls. This dramatically
    // reduces eviction traffic in SIMD-heavy blocks that fall back to
    // the interpreter frequently.
    static constexpr int NUM_ALLOC_REGS = 9;
    static constexpr int ALLOC_REGS[9] = {RAX, RCX, RDX, R8, R9, R11, R12, R13, R15};

    // Returns true if `r` is caller-saved (clobbered by C calls).
    // R12/R13/R15 are callee-saved → preserved across calls.
    static constexpr bool is_caller_saved(int r) {
        return r == RAX || r == RCX || r == RDX ||
               r == R8  || r == R9  || r == R11 ||
               r == R10;  // WIN_REG is caller-saved too
    }

    // vreg → x86 reg (or -1 if spilled to stack).
    int vreg_home_[4096];
    // x86 reg → vreg currently in it (or -1).
    int reg_vreg_[16];
    // vreg → dirty (needs writeback to cpu.regs[]/stack on spill/flush).
    bool vreg_dirty_[4096];
    // vreg → stack slot offset (or 0 if not yet spilled).
    int32_t vreg_slot_[4096];
    // Number of stack slots used.
    int num_stack_slots_ = 0;
    // Max vreg used in this block.
    int max_vreg_ = 0;

    int  alloc_reg(int preferred = -1);
    void evict_vreg(int v);
    void flush_all_vregs();
    void invalidate_all_vregs();
    // v1.4.0-alpha.5: flush/invalidate only caller-saved vregs. Used
    // around CALL_INTERP and memory ops — callee-saved vregs (R12/R13/
    // R15) are preserved by the C calling convention, so they DON'T
    // need to be spilled or invalidated. This keeps live values in
    // callee-saved regs across interpreter calls, eliminating redundant
    // reload traffic.
    void flush_caller_saved_vregs();
    void invalidate_caller_saved_vregs();
    int  ensure_vreg(int v, int preferred = -1);
    void set_vreg_reg(int v, int r);
    void kill_vreg(int v);
    int32_t vreg_stack_slot(int v);
    int  alloc_reg_for(int v, int preferred = -1);  // alloc + evict old occupant BEFORE computation

    // Force a vreg into a specific host register (MOVE semantics).
    // Evicts the current occupant of `host_reg` if any, then either
    // moves `v` from its current home (clearing the old mapping) or
    // loads it from memory. After this call:
    //   vreg_home_[v] == host_reg, reg_vreg_[host_reg] == v.
    // Use this when the caller needs `v` in a specific reg AND doesn't
    // need `v` to remain in its old location.
    void force_vreg_to_reg(int v, int host_reg);

    // Force two vregs into two specific host registers in one call.
    // Handles the aliasing case where src1 == src2 (or src2 was
    // originally cached in host_reg1) by COPYING src2 to host_reg2
    // instead of moving (so src1's mapping in host_reg1 is preserved).
    // After this call:
    //   vreg_home_[src1] == host_reg1, reg_vreg_[host_reg1] == src1.
    //   vreg_home_[src2] == host_reg2, reg_vreg_[host_reg2] == src2.
    // Use this for binary ops like SHL/ADDS that need src1 in one
    // fixed reg and src2 in another (e.g. RAX and RCX).
    void force_two_vregs_to(int src1, int host_reg1,
                            int src2, int host_reg2);

    // ── FMOV helper ───────────────────────────────────────────────────
    // Moves a 64-bit value between a GPR vreg and an FP register slot
    // (cpu.v_lo[] or cpu.v_hi[]) via the RAX scratch register.
    //
    //   dir = 0: GPR → FP,  fp_field = 0 (v_lo) or 1 (v_hi)
    //            Stores src1 vreg into the FP slot. If fp_field == 0
    //            (FMOV_G2F), also zeros v_hi[dest] — ARM semantics.
    //   dir = 1: FP → GPR, fp_field = 0 (v_lo) or 1 (v_hi)
    //            Loads the FP slot into a fresh vreg for dest.
    //
    // `idx` is the FP register index (0-31) — comes from inst.dest for
    // G→F or inst.src1 for F→G.
    //
    // After G→F: RAX is clobbered; its cache mapping is dropped safely
    // (capture-before-clear, avoiding the OOB write that previously
    // lived inline in each FMOV_G2F* case).
    void emit_fmov_helper(int dir, int fp_field, uint16_t idx,
                          uint16_t src1, uint16_t dest);

    // Old simple load/store (kept for fallback).
    void load_vreg(int dst, int v);
    void store_vreg(int v, int src);

    // ── Typed emit_call_abs overload ──────────────────────────────────
    // The void* overload (declared above) is the low-level primitive.
    // This function-pointer overload lets call sites write:
    //     emit_call_abs(&jit_interp_step);
    //     emit_call_abs(jit_load_mem_slow);
    // instead of:
    //     emit_call_abs((void*)&jit_interp_step);
    //     emit_call_abs((void*)&jit_load_mem_slow);
    // C++ overload resolution picks this template when the argument is
    // a function pointer; the void* overload is picked for void*.
    template <typename R, typename... Args>
    void emit_call_abs(R (*fn)(Args...)) {
        emit_call_abs(reinterpret_cast<void*>(fn));
    }

    // IR compiler helpers.
    struct BranchPatch { size_t patch_off; int target_kind; };
    void emit_call_interp(uint64_t arm_pc, bool ends_block);
    bool compile_ir_inst(const IRInst& inst);
    // Emit a frameless jcc/jmp to a loop-top block's body. Returns true
    // if emitted (caller skips normal epilogue). See implementation.
    bool emit_frameless_back_edge(uint64_t target_pc, uint8_t cc);
    // Patch a pending back-edge site (recorded in pending_back_edges_)
    // to jump directly to the now-translated target's body.
    void patch_pending_back_edges(uint64_t target_pc);

    // Per-block state (reset at translate_block start).
    std::vector<size_t> call_interp_branch_patches_;
    std::vector<BranchPatch> branch_target_patches_;
    bool rax_holds_next_pc_ = false;
    bool flags_in_host_ = false;
    bool flags_from_sub_ = false;

    // Block-chaining state (reset at translate_block start).
    // chain_target_pc_ > 0 means the block's statically-known next PC
    // (suitable for chaining). unchainable_end_ = true means the block
    // ended with an op whose next PC is runtime-dependent (BR/BRCOND/SVC),
    // so it cannot be chained even at fall-through.
    uint64_t chain_target_pc_ = 0;
    bool unchainable_end_ = false;

    // ── Frameless back-edge chaining ──────────────────────────────────
    // When a conditional/unconditional branch targets a PC ≤ start_pc
    // (a loop back-edge), we can emit a direct jcc/jmp to the target
    // block's BODY (skipping its prologue), provided:
    //   - the target block is already translated
    //   - the target block is "frameless_compatible" (its body doesn't
    //     rely on a fresh stack frame beyond what the loop top already
    //     has set up)
    //   - we flush all dirty architectural vregs + materialize flags
    //     before the jump (the target will reload from cpu.regs[]/
    //     pstate, so we must write them back)
    //
    // Each entry is a back-edge site that needs patching. `patch_off` is
    // the offset of the jcc/jmp rel32 placeholder; `target_pc` is the
    // loop-top PC; `is_conditional` distinguishes jcc (6 bytes) from
    // jmp (5 bytes). `taken_path_off` is the offset where the "taken"
    // path begins (for conditional back-edges, this is right after the
    // jcc; for unconditional, it IS the jmp).
    struct BackEdgePatch {
        size_t  patch_off;       // offset of the rel32 to patch
        uint64_t target_pc;      // loop-top PC
        bool    is_conditional;  // jcc (6 bytes) vs jmp (5 bytes)
    };
    std::vector<BackEdgePatch> back_edge_patches_;
    // Per-PC list of back-edge sites that target this PC, kept across
    // block translations so a later-translated loop top can patch
    // earlier-emitted back-edges. Key = target_pc, value = list of sites.
    std::unordered_map<uint64_t, std::vector<BackEdgePatch>> pending_back_edges_;

    // Materialize pending host flags to pstate if any flag-clobbering
    // instruction is about to execute. Called by ADD/SUB/AND/OR/XOR/
    // SHL/SHR/SAR/ROR/NOT/NEG/IMUL to preserve flag correctness.
    void clobber_flags();

    // Translation entry point.
    uint64_t (*translate_block(Emulator& emu, uint64_t start_pc))(CPU*, Emulator*);
};

} // namespace arm64emu
