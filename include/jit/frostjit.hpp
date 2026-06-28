// frostjit.hpp — IR → x86-64 JIT for bifrost-emu ()
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
#include "ir/ir.hpp"
#include "jit/cpu_features.hpp"
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

    // ── Function Multi-Versioning (FMV) ─────────────────────────────
    // The JIT queries these flags at codegen time to decide which x86
    // instruction sequence to emit for hot operations. For example,
    // FMADD/FMSUB/FNMADD/FNMSUB can use the FMA3 three-operand VEX
    // encoding (vfmadd231ss/sd) when the host CPU supports it, giving
    // both correctness (true single-rounded fused mul-add per IEEE 754)
    // and ~1 cycle/insn savings. On CPUs without FMA3, the JIT falls
    // back to the existing decomposed mulsd+addsd sequence.
    //
    // Detection runs once per FrostJIT (at construction). The result
    // is cached for the JIT's lifetime — CPU features don't change at
    // runtime. Override via BIFROST_NO_FMA3=1 to force the decomposed
    // path even on FMA3-capable CPUs (debugging).
    const CpuFeatures& cpu_features() const { return cpu_features_; }
    bool has_fma3() const { return cpu_features_.has_fma3() && !no_fma3_; }

    uint64_t blocks_translated = 0;
    uint64_t blocks_executed   = 0;
    uint64_t instructions_executed = 0;  // sum of instr_count over executed blocks
    uint64_t cache_hits        = 0;
    uint64_t cache_misses      = 0;
    uint64_t interpreter_fallbacks = 0;
    uint64_t block_chains_patched = 0;

    // Loop watchdog state — per-instance so multiple FrostJIT objects
    // (e.g. one per thread) don't share/corrupt each other's counters.
    // Resets on any different PC; if the same PC runs > WATCHDOG_LIMIT
    // times in a row, fall back to the interpreter to break the loop.
    // Raised from 100K to 500M: pure JIT blocks (no CALL_INTERPs) are no
    // longer demoted to interp_only, so tight loops legitimately run
    // 100M+ iterations through the dispatcher before self-loop chaining
    // kicks in.
    static constexpr uint32_t WATCHDOG_LIMIT = 500000000;
    uint64_t watchdog_last_pc_ = UINT64_MAX;
    uint32_t watchdog_count_   = 0;

    // v1.4.0-beta.2: Per-PC hotness counter. Tracks how many times each
    // PC has been dispatched (total, not consecutive). When a PC exceeds
    // HOT_PC_THRESHOLD, it's marked interp_only — the interpreter is
    // faster for tiny blocks because it skips the C dispatcher overhead
    // (~1us per block). This catches tight multi-block cycles (e.g.,
    // __multf3's ~10-block cycle) that the consecutive-PC watchdog can't
    // detect. The counter map is bounded by HOT_PC_MAP_MAX to prevent
    // unbounded memory growth; eviction is LRU-ish (clear on overflow).
    static constexpr uint32_t HOT_PC_THRESHOLD = 5000;
    static constexpr size_t   HOT_PC_MAP_MAX   = 65536;
    std::unordered_map<uint64_t, uint32_t> hot_pc_counts_;

    // Global progress watchdog: if total block executions exceed this
    // limit, the JIT switches to interpreter-only mode permanently.
    // This is a safety valve for JIT codegen bugs that cause infinite
    // loops across multiple PCs. Set high enough that real workloads
    // (toybox, musl libc loops) never trip it — 10000 was way too low
    // and disabled the JIT mid-run on any non-trivial program.
    // v1.4.0-beta.2: bumped from 50M to 1B. Soft-float-heavy programs
    // (long double multiply, printf %Lf) can legitimately dispatch
    // 100M+ tiny interp_only blocks; 50M was too aggressive.
    static constexpr uint64_t GLOBAL_BLOCK_LIMIT = 1000000000;
    uint64_t total_blocks_executed_ = 0;
    bool     jit_disabled_ = false;  // set by global watchdog

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

    // Total number of host GPRs (RAX..R15). Used by the register
    // allocator's bounds checks and the dirty_host_regs_ bitmask. The
    // old code hardcoded `16` in multiple places (x86_regalloc.cpp:66,
    // jit_profiler.cpp:87, frostjit.cpp:2817) — centralizing here means
    // a future change (e.g. adding XMM regs to the allocator) only
    // needs one edit.
    static constexpr int NUM_HOST_REGS = 16;
    // Dirty-bitmask width must match NUM_HOST_REGS. uint16_t holds 16 bits.
    static_assert(NUM_HOST_REGS <= 16, "dirty_host_regs_ is uint16_t; "
                  "NUM_HOST_REGS must be <= 16");

private:
    static constexpr size_t CODE_BUF_SIZE = 64 * 1024 * 1024;
    uint8_t* code_buf_ = nullptr;
    size_t   code_buf_used_ = 0;
    bool     code_buf_overflow_ = false;
    uint8_t* window_base_ = nullptr;

    // ── CPU features (FMV) ──────────────────────────────────────────
    // Detected once at construction via CPUID + XGETBV. Cached for the
    // JIT's lifetime. Polled by compile_ir_inst() when emitting code
    // for hot operations that have multiple x86 codegen variants.
    CpuFeatures cpu_features_{};
    bool no_fma3_ = false;  // true if BIFROST_NO_FMA3=1 (force decomposed path)

    // ── W^X (Write XOR Execute) protection ──────────────────────────
    // The code buffer is mapped PROT_READ|PROT_EXEC by default (no WRITE).
    // Before any codegen or patching operation, call make_writable() to
    // toggle the buffer to PROT_READ|PROT_WRITE. After the write, call
    // make_executable() to restore PROT_READ|PROT_EXEC.
    //
    // This prevents code-injection attacks where a buffer overflow in the
    // JIT (or a bug in the decoder/codegen) could write malicious x86
    // instructions into the executable buffer and have them run. With W^X,
    // the buffer is never simultaneously writable and executable.
    //
    // Reference counting: make_writable() increments wex_write_depth_;
    // make_executable() decrements it. The buffer only becomes executable
    // when wex_write_depth_ reaches 0. This allows patch_chain to be called
    // from within translate_block without prematurely toggling the buffer
    // to RX while translate_block is still emitting code.
    //
    // Disabled if BIFROST_NO_WEX=1 is set in the environment (for
    // performance-sensitive builds where the security tradeoff is
    // acceptable). On systems where mprotect fails (e.g., some hardened
    // kernels), W^X is automatically disabled and the buffer falls back
    // to RWX.
    bool     wex_enabled_ = false;
    int      wex_write_depth_ = 0;  // >0 means buffer is currently writable
    void make_writable();   // mprotect(code_buf_, RW) — call before writes
    void make_executable(); // mprotect(code_buf_, RX) — call before execution

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
        bool    interp_only = false;  // true if block is too CALL_INTERP-heavy to JIT — run via interpreter
        int     interp_only_count = 0; // number of ARM instructions to step for interp_only blocks
        int     call_interp_count = 0; // number of CALL_INTERP fallbacks in this block
        // Self-loop chaining: when the block's BRCOND taken target equals its
        // own start PC, a 5-byte `jmp rel32` slot is emitted on the taken path.
        // After the block is fully compiled, translate_block patches this slot
        // to jump directly to the block body start — skipping the epilogue,
        // dispatcher, and prologue. The block body runs again immediately.
        // This is the single biggest win for tight loops (e.g. bench_mips):
        // ~25 instructions of per-iteration overhead are eliminated.
        // has_selfloop_slot = false means no slot was emitted (block is not
        // a self-loop, or self-loop chaining is disabled via BIFROST_NO_SELFLOOP).
        bool    has_selfloop_slot = false;
        size_t  selfloop_patch_off = 0;  // offset of the 5-byte jmp slot in code_buf_
        // Verify-mode: set true after the first BIFROST_JIT_VERIFY dispatch
        // of this block. Subsequent dispatches skip the per-block divergence
        // check (which is expensive due to mprotect toggling + interpreter
        // replay). This is essential for self-loop blocks, where verify mode
        // must un-patch the self-loop slot to run one iteration at a time —
        // without this flag, every loop iteration would pay the verify
        // overhead (~30s for a 3652-instruction test instead of <1s).
        // First-dispatch verify still catches real codegen bugs because
        // divergences almost always manifest on the first execution with
        // any input values. The flag is only consulted when verify mode
        // is active.
        bool    verified_once = false;
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
    void emit_lzcnt_reg(int dst, int src);
    void emit_bswap_reg(int dst);
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
    // After emit_load_flags_from_pstate, x86 CF = ARM C XOR from_sub.
    // This helper emits runtime code to normalize CF to SUB convention
    // (x86 CF = NOT ARM C) by inverting CF when from_sub=0. After this,
    // the default arm_cond_to_x86() mapping (which assumes SUB convention)
    // is correct for all conditions.
    // Uses RAX and RCX as scratch (caller must ensure they're free).
    void emit_normalize_cf_to_sub_convention();

    // Memory access (direct-window path).
    void emit_load_mem(int dst, int addr_reg, int32_t off, int w, bool sign_ext);
    void emit_store_mem(int addr_reg, int32_t off, int src_reg, int w);

    // Move a 64-bit immediate into RAX, using the 32-bit zero-extend form
    // when the value fits in 32 bits (smaller code).
    void emit_mov_imm_to_rax(uint64_t val);

    // Resolve an ARM condition code to an x86 Jcc condition code, handling
    // the carry-polarity difference between ADD/TST (direct CF) and SUB
    // (inverted CF). Sets need_cmc=true if the caller must emit a `cmc`
    // before the JCC (needed for HI/LS after ADD/TST).
    uint8_t resolve_arm_cond_with_carry(uint8_t arm_cond, bool& need_cmc);

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
    // The pool includes R12/R13/R15 (callee-saved). This gives 9
    // registers instead of 6, and vregs cached in callee-saved regs
    // survive CALL_INTERP without spilling — the C calling convention
    // preserves them across calls. This dramatically reduces eviction
    // traffic in SIMD-heavy blocks that fall back to the interpreter
    // frequently.
    static constexpr int NUM_ALLOC_REGS = 9;
    static constexpr int ALLOC_REGS[9] = {RAX, RCX, RDX, R8, R9, R11, R12, R13, R15};

    // Returns true if `r` is caller-saved (clobbered by C calls).
    // R12/R13/R15 are callee-saved → preserved across calls.
    static constexpr bool is_caller_saved(int r) {
        return r == RAX || r == RCX || r == RDX ||
               r == R8  || r == R9  || r == R11 ||
               r == R10;  // WIN_REG is caller-saved too
    }

    // Bitmask of all caller-saved host regs (used as a fast `mask` arg).
    static constexpr uint16_t CALLER_SAVED_MASK =
        (1u << RAX) | (1u << RCX) | (1u << RDX) |
        (1u << R8)  | (1u << R9)  | (1u << R11) | (1u << R10);

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
    // Max vreg from the previous block — used to bound the array-clearing
    // in translate_block() so we don't zero all 4096 entries every time.
    int prev_max_vreg_ = 0;

    // ── FP register index validation ───────────────────────────────
    // FP ops (FP_BINOP, FP_UNOP, FP_F2I, FP_I2F, FP_CMP, FP_MOVI, FMADD,
    // SIMD_*) use inst.dest/src1/src2 as FP register indices (0-31).
    // A decoder or IR-translator bug could produce indices > 31, causing
    // out-of-bounds writes to the CPU struct. This check catches it.
    // Active in debug builds or with BIFROST_REGALLOC_CHECK=1.
    void check_fp_reg_index(int idx, const char* context) const;

    // ── Dirty host-reg bitmask (unique flush-reduction scheme) ───────
    // Bit `r` is set iff reg_vreg_[r] holds a dirty vreg (i.e.
    // vreg_dirty_[reg_vreg_[r]] == true). Maintained in lockstep with
    // set_vreg_reg / alloc_reg_for / evict_vreg / kill_vreg / drop_vreg /
    // clobber_host_reg / invalidate_all_vregs. Lets us do O(popcount(mask))
    // targeted flushes instead of O(max_vreg_) scans — critical for FP
    // heavy blocks where max_vreg_ can be 256+.
    //
    // Invariant:
    //   dirty_host_regs_ & (1u << r)  ⇔  reg_vreg_[r] >= 0 &&
    //                                     vreg_dirty_[reg_vreg_[r]]
    uint16_t dirty_host_regs_ = 0;

    int  alloc_reg(int preferred = -1);
    // Allocate a host reg, but never return `excl1` or `excl2`.
    // Used by the ALU codegen to ensure dest doesn't collide with src1/src2's
    // host regs. Scans for a free reg (skipping the excluded ones); if all are
    // occupied, evicts a non-excluded reg.
    int  alloc_reg_excluding(int excl1, int excl2);
    void evict_vreg(int v);
    void drop_vreg(int v);  // safe drop — evicts if dirty (use instead of raw clear)
    // Evict the occupant of `host_reg` if it's dirty, then clear the mapping.
    // Use this BEFORE clobbering `host_reg` with a computation that doesn't
    // care about the old value (e.g. emit_mov_imm64(RAX, ...) in FP_MOVI).
    // Without this, a dirty vreg cached in `host_reg` is silently lost.
    void clobber_host_reg(int host_reg);
    void flush_all_vregs();
    void invalidate_all_vregs();

    // ── Targeted flush/invalidate (v1.4.0-beta.2) ───────────────────
    // Walk only the host regs whose bits are set in `mask`, spilling any
    // dirty vreg cached there. O(popcount(mask)) instead of O(max_vreg_).
    // Used by FP JIT codegen — FP ops only clobber XMM0/XMM1 plus a small
    // fixed set of GPRs (RAX/RCX/RDX), so flushing just those is enough.
    void flush_dirty_host_regs(uint16_t mask);
    // Spill ALL scratch vregs (v > 31) cached in host regs selected by
    // `mask`, regardless of dirty status. Arch vregs (v <= 31) are NOT
    // spilled — their value is also in cpu.regs[], so dropping the cache
    // mapping is safe (load_vreg_to_reg reloads from cpu.regs[]).
    //
    // This is needed BEFORE operations that clobber host regs (like
    // emit_materialize_flags or the MEM_CLOBBER set in LOAD_MEM/STORE_MEM)
    // when those host regs hold non-dirty scratch vregs. Without this,
    // the non-dirty scratch vreg's value is lost — it was computed in the
    // host reg but never written to its stack slot, and the clobbering op
    // destroys it before invalidate_host_regs drops the mapping.
    void flush_scratch_host_regs(uint16_t mask);
    // Drop cache mappings for host regs in `mask` (no spill — caller must
    // have already flushed if any were dirty). Companion to above.
    void invalidate_host_regs(uint16_t mask);
    // Convenience: flush + invalidate in one call (the common pattern).
    // Spills dirty vregs AND non-dirty scratch vregs before invalidating.
    inline void flush_invalidate_host_regs(uint16_t mask) {
        flush_dirty_host_regs(mask);
        flush_scratch_host_regs(mask);
        invalidate_host_regs(mask);
    }
    // Debug-only: verify the dirty_host_regs_ invariant. Returns true if OK.
    bool verify_dirty_host_regs_() const;

    int  ensure_vreg(int v, int preferred = -1);
    void set_vreg_reg(int v, int r);
    void kill_vreg(int v);
    int32_t vreg_stack_slot(int v);
    int  alloc_reg_for(int v, int preferred = -1);  // alloc + evict old occupant BEFORE computation

    // ── Codegen helpers (reduce boilerplate in compile_ir_inst) ──────
    // Load vreg `v` into host reg `dst`, handling both arch vregs (0-31,
    // loaded from cpu.regs[]) and scratch vregs (33+, loaded from stack).
    // Does NOT participate in the cache — use ensure_vreg for that.
    void load_vreg_to_reg(int dst, int v);
    // Store host reg `src` to arch reg `v` (0-31 = cpu.regs[], 31 = sp).
    void store_reg_to_vreg(int v, int src);

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
    // (cpu.v_lo[] or cpu.v_hi[]) via RAX (G→F) or a fresh reg (F→G).
    //
    //   dir = 0: GPR → FP,  fp_field = 0 (v_lo) or 1 (v_hi)
    //            Stores src1 vreg into the FP slot. If fp_field == 0
    //            (FMOV_G2F), also zeros v_hi[dest] — ARM semantics.
    //            Uses RCX as scratch for the zero store so src1 stays
    //            cached in RAX for later readers.
    //   dir = 1: FP → GPR, fp_field = 0 (v_lo) or 1 (v_hi)
    //            Loads the FP slot into a fresh vreg for dest.
    //
    // `idx` is the FP register index (0-31) — comes from inst.dest for
    // G→F or inst.src1 for F→G.
    void emit_fmov_helper(int dir, int fp_field, uint16_t idx,
                          uint16_t src1, uint16_t dest);

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

    // Per-block state (reset at translate_block start).
    std::vector<size_t> call_interp_branch_patches_;
    std::vector<BranchPatch> branch_target_patches_;
    bool rax_holds_next_pc_ = false;
    bool flags_in_host_ = false;
    bool flags_from_sub_ = false;

    // ── Liveness-based register freeing ─────────────────────────────
    // kills_per_op_[i] = list of scratch vregs whose last use is IR op i
    // (and that are not the dest of op i). After compiling op i, each
    // vreg in this list is killed via kill_vreg, freeing its host reg for
    // reuse without eviction. Only scratch vregs (33+) are tracked — ARM
    // reg vregs (0-31) represent architectural state that must be flushed
    // at the epilogue, so they must not be killed early.
    std::vector<std::vector<uint16_t>> kills_per_op_;

    // Block-chaining state (reset at translate_block start).
    // chain_target_pc_ > 0 means the block's statically-known next PC
    // (suitable for chaining). unchainable_end_ = true means the block
    // ended with an op whose next PC is runtime-dependent (BR/BRCOND/SVC),
    // so it cannot be chained even at fall-through.
    uint64_t chain_target_pc_ = 0;
    bool unchainable_end_ = false;

    // Self-loop chaining state (reset at translate_block start).
    // When a BRCOND's taken target == block start PC, a 5-byte jmp slot is
    // emitted on the taken path. translate_block patches it to jump directly
    // to the block body start (block_body_start_off_), creating a tight loop
    // that skips the epilogue/dispatcher/prologue.
    bool    has_selfloop_slot_ = false;
    size_t  selfloop_patch_off_ = 0;     // offset of the 5-byte jmp slot
    size_t  block_body_start_off_ = 0;   // offset of block body (after prologue)
    uint64_t current_start_pc_ = 0;      // start PC of the block being translated

    // Materialize pending host flags to pstate if any flag-clobbering
    // instruction is about to execute. Called by ADD/SUB/AND/OR/XOR/
    // SHL/SHR/SAR/ROR/NOT/NEG/IMUL to preserve flag correctness.
    void clobber_flags();

    // Materialize host flags (NZCV) to cpu.pstate, preserving the current
    // RFLAGS around the materialization (which clobbers RAX/RCX/RDX).
    // Used at block exits (BRCOND fall-through and taken paths) where the
    // next block may read pstate. No-op if flags_in_host_ is false.
    // Does NOT clear flags_in_host_ — the caller manages that.
    void materialize_flags_to_pstate();

    // Translation entry point.
    uint64_t (*translate_block(Emulator& emu, uint64_t start_pc))(CPU*, Emulator*);
};

} // namespace arm64emu
