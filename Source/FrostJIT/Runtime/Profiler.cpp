// Profiler.cpp — FrostJIT lifecycle + statistics + watchdog.
//
// Holds construction/destruction (mmap of the code buffer), cache flush,
// and the per-instance watchdog state (reset on construction; checked
// in run_block). Statistics counters (blocks_translated, blocks_executed,
// cache_hits, cache_misses, interpreter_fallbacks, block_chains_patched)
// are public fields on FrostJIT; this file owns their initialization.
//
// Watchdog:
//   - Per-instance loop watchdog: if the same PC runs > WATCHDOG_LIMIT
//     times consecutively, fall back to the interpreter for that block
//     (and mark it interp_only permanently).
//   - Global progress watchdog: if total block executions exceed
//     GLOBAL_BLOCK_LIMIT, the JIT switches to interpreter-only mode
//     permanently (safety valve for codegen-bug-induced infinite loops).
//
// Verify mode (BIFROST_JIT_VERIFY env var) is implemented inline in
// run_block (FrostJIT.cpp) because it needs tight coupling with the
// block dispatch path.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/DebugFlags.hpp"
#include "Bifrost/Version.hpp"  // CODENAME
#include "FrostJIT/Runtime/SampleProfile.hpp"
#include <sys/mman.h>
#include <cerrno>
#include <cstring>   // memcpy
#include <cstdlib>   // getenv
#include <algorithm> // sort
#include <cstdio>    // fprintf
#include <map>
#include <stdexcept>
namespace arm64emu {
// ── Construction ────────────────────────────────────────────────────────
FrostJIT::FrostJIT() {
    dispatch_stats_enabled_ = dbg().jit_dispatch_stats;
    code_buf_limit_ = dbg().jit_cache_mb * 1024 * 1024;
    code_capacity_ = std::min<size_t>(dbg().jit_cache_initial_mb * 1024 * 1024, code_buf_limit_);
    // W^X (Write XOR Execute) protection: allocate the code buffer as
    // PROT_READ|PROT_WRITE first (for codegen), then toggle to
    // PROT_READ|PROT_EXEC before execution. This prevents the buffer
    // from being simultaneously writable and executable, mitigating
    // code-injection attacks via JIT bugs.
    //
    // We check BIFROST_NO_WEX=1 to disable (for perf-sensitive builds).
    // If the initial mprotect to RX fails (some hardened kernels reject
    // PROT_EXEC on anonymous mappings), we fall back to RWX.
    bool disable_wex = (getenv("BIFROST_NO_WEX") != nullptr);
    int initial_prot = disable_wex
        ? (PROT_READ | PROT_WRITE | PROT_EXEC)
        : (PROT_READ | PROT_WRITE);
    void* p = mmap(nullptr, code_buf_limit_, initial_prot,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "[%s] frostJIT: mmap code buffer failed (%zu bytes): %s\n",
                CODENAME, code_buf_limit_, strerror(errno));
        code_buf_ = nullptr;
    } else {
        code_buf_ = static_cast<uint8_t*>(p);
    }
    // Initialize W^X state. If we started RW (no EXEC), enable W^X and
    // mark the buffer as currently writable (since we just allocated it
    // RW and will write to it during the first translate_block).
    if (code_buf_ && !disable_wex) {
        wex_enabled_ = true;
        wex_write_depth_ = 0;  // buffer is RX (not writable) by default
    }
    // Counters start at zero (declared in the public header).
    blocks_translated = 0;
    blocks_executed   = 0;
    cache_hits        = 0;
    cache_misses      = 0;
    interpreter_fallbacks = 0;
    block_chains_patched = 0;
    // Watchdog state (thread-local — reset for this thread).
    tls_watchdog_last_pc_ = UINT64_MAX;
    tls_watchdog_count_   = 0;
    total_blocks_executed_.store(0, std::memory_order_relaxed);
    jit_disabled_.store(false, std::memory_order_relaxed);
    // Initialize vreg arrays — prev_max_vreg_ must be large enough that
    // the first translate_block() clears all 4096 entries. Without this,
    // uninitialized vreg_home_[] garbage causes load_vreg_to_reg to think
    // vregs are cached in random host regs, producing mov-from-garbage.
    prev_max_vreg_ = 4095;
    for (int i = 0; i < 4096; i++) {
        vreg_home_[i] = -1;
        vreg_dirty_[i] = false;
        vreg_slot_[i] = 0;
        vreg_last_use_[i] = 0;  // reset LRU timestamps
    }
    for (int i = 0; i < NUM_HOST_REGS; i++) reg_vreg_[i] = -1;
    max_vreg_ = 0;
    // ── FMV: detect CPU features once at construction ──────────────
    // The result is cached for the JIT's lifetime — CPU features don't
    // change at runtime. Polled by compile_ir_inst() when emitting code
    // for hot operations with multiple x86 codegen variants (currently
    // only FMADD/FMSUB/FNMADD/FNMSUB → FMA3 vs. decomposed mul+add/sub).
    //
    // BIFROST_NO_FMA3=1 forces the decomposed path even on FMA3 CPUs.
    // This is a debugging aid: it lets us A/B-test the FMA3 codegen
    // against the decomposed codegen on the same machine, and it gives
    // users a workaround if FMA3 codegen has a bug we haven't found yet.
    cpu_features_ = detect_cpu_features();
    no_fma3_ = (getenv("BIFROST_NO_FMA3") != nullptr);
    no_avx2_ = (getenv("BIFROST_NO_AVX2") != nullptr);
    init_fault_recovery();
}
FrostJIT::~FrostJIT() {
    destroy_fault_recovery();
    if (code_buf_ && dbg().cpu_profile) prof_release_code(code_buf_, code_buf_limit_);
    if (code_buf_) munmap(code_buf_, code_buf_limit_);
}
// ── W^X protection toggle (reference-counted) ──────────────────────────
// make_writable: increment the write depth. If this is the first writer
// (depth was 0), mprotect the buffer to RW. Subsequent calls are no-ops
// (the buffer is already writable). This allows nested calls like
// translate_block → patch_chain without premature make_executable.
void FrostJIT::make_writable() {
    if (!wex_enabled_) return;
    if (wex_write_depth_ == 0) {
        // First writer: toggle buffer from RX to RW.
        if (mprotect(code_buf_, code_buf_limit_, PROT_READ | PROT_WRITE) != 0) {
            // Try RWX at the same address; moving code is unsafe.
            wex_enabled_ = false;
            // Keep the address stable: published code contains relative
            // calls and is retained by TLS caches and live call frames.
            if (mprotect(code_buf_, code_buf_limit_,
                         PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
                jit_disabled_.store(true, std::memory_order_relaxed);
                fprintf(stderr, "[%s] frostJIT: cannot make stable code buffer writable: %s\n",
                        CODENAME, strerror(errno));
                throw std::runtime_error("JIT code buffer protection failed");
            }
            return;
        }
    }
    wex_write_depth_++;
}
// make_executable: decrement the write depth. If this is the last writer
// (depth reaches 0), mprotect the buffer to RX. No-op if other writers
// are still active (nested calls).
void FrostJIT::make_executable() {
    if (!wex_enabled_) return;
    if (wex_write_depth_ <= 0) return;  // defensive: never go negative
    wex_write_depth_--;
    if (wex_write_depth_ == 0) {
        // Last writer done: toggle buffer from RW to RX.
        mprotect(code_buf_, code_buf_limit_, PROT_READ | PROT_EXEC);
        // If mprotect fails, leave the buffer writable (better than crashing).
    }
}
uint8_t FrostJIT::profile_op_kind(IROp op) {
    switch (op) {
    case IROp::LOAD_REG: case IROp::STORE_REG:
        return PROF_STATE;
    case IROp::LOAD_MEM: case IROp::STORE_MEM: case IROp::SIMD_LDST: case IROp::SIMD_LD16: case IROp::SIMD_ST16:
    case IROp::SIMD_STRUCT_MEM:
    case IROp::MEMORY_BARRIER: case IROp::CLEAR_EXCLUSIVE: case IROp::ATOMIC: case IROp::LDXR_FAST: case IROp::STXR_FAST:
    case IROp::STLR_FAST:
        return PROF_MEMORY;
    case IROp::ADDS: case IROp::SUBS: case IROp::ADCS: case IROp::SBCS: case IROp::TST: case IROp::TST_ZERO: case IROp::CCMP:
        return PROF_FLAGS;
    case IROp::BR: case IROp::BRCOND: case IROp::BRCOND_FALLTHRU: case IROp::BRCOND_SKIP: case IROp::BRCOND_ZERO:
    case IROp::BRCOND_BIT:
        return PROF_BRANCH;
    case IROp::BL_CALL: case IROp::BLR_CALL: case IROp::CALL_INTERP: case IROp::SVC:
        return PROF_CALL;
    case IROp::FMOV_G2F: case IROp::FMOV_F2G: case IROp::FMOV_G2FHI: case IROp::FMOV_FHI2G: case IROp::FP_BINOP:
    case IROp::FP_UNOP: case IROp::FP_MOV: case IROp::SIMD_LOGICAL: case IROp::SIMD_NATIVE_STRUCT: case IROp::SIMD_NATIVE_EXTRA:
    case IROp::SIMD_SCALAR_DUP: case IROp::SIMD_DUP: case IROp::SIMD_UMOV: case IROp::SIMD_SMOV: case IROp::SIMD_SATADDSUB:
    case IROp::SIMD_MOVI: case IROp::SIMD_ORRIMM: case IROp::SIMD_ARITH: case IROp::SIMD_CMP: case IROp::SIMD_SHL:
    case IROp::SIMD_USHR: case IROp::SIMD_SSHR: case IROp::SIMD_USRA: case IROp::SIMD_SSRA: case IROp::SIMD_URSRA:
    case IROp::SIMD_SRSRA: case IROp::SIMD_SLI: case IROp::SIMD_SRI: case IROp::SIMD_FP_ARITH: case IROp::SIMD_FP_UNARY:
    case IROp::SIMD_FP_FMA: case IROp::SIMD_2REG: case IROp::SIMD_CVTF: case IROp::SIMD_ADDP: case IROp::SIMD_XTN:
    case IROp::SIMD_TBL: case IROp::SIMD_INS: case IROp::SIMD_PERMUTE: case IROp::SIMD_PAIRMIN: case IROp::SIMD_ABDL:
    case IROp::SIMD_ABD: case IROp::SIMD_ADDW: case IROp::SIMD_ADDHN: case IROp::SIMD_SHRN_SAT: case IROp::SIMD_MUL_ELEM:
    case IROp::FP_F2I: case IROp::FP_I2F: case IROp::FP_F2I_FIXED: case IROp::FP_I2F_FIXED: case IROp::FP_CMP:
    case IROp::FP_MOVI: case IROp::FCVT_S2D: case IROp::FCVT_D2S: case IROp::FRINT: case IROp::FMADD: case IROp::FMSUB:
    case IROp::FNMADD: case IROp::FNMSUB: case IROp::AES_CRYPTO: case IROp::FP_CSEL:
        return PROF_FPSIMD;
    case IROp::NOP: case IROp::MRS: case IROp::MSR:
        return PROF_OTHER;
    default:
        return PROF_ALU;  // integer arithmetic, logic, shifts, moves, constants, conditional selects, division
    }
}

void FrostJIT::note_profile_sections(size_t begin, size_t hot_end, size_t cold_begin) {
    if (!profile_sections_active_ || code_buf_overflow_) return;
    const size_t body = std::min(profile_body_begin_ == SIZE_MAX ? hot_end : profile_body_begin_, hot_end);
    const uint64_t base = reinterpret_cast<uint64_t>(code_buf_);
    prof_note_block({base + begin, base + body, base + hot_end, base + cold_begin, base + code_buf_used_, current_start_pc_});
    std::vector<ProfRange> ranges;
    ranges.reserve(profile_ranges_.size());
    for (const ProfileRange& r : profile_ranges_) ranges.push_back({base + r.begin, base + r.end, r.op, r.spill});
    prof_note_ranges(ranges.data(), ranges.size());
}

void FrostJIT::flush_cache() {
    // Clear block metadata. We don't need writable access for this —
    // blocks_/back_refs_/hot_pc_counts_ are STL containers, not the code
    // buffer. The old code called make_writable() here, leaving
    // wex_write_depth_=1 (unbalanced). The next translate_block would
    // then run with the buffer in RW state until its make_executable()
    // at the end — a security hole (W^X violated during execution).
    //
    // Fix: don't touch wex_write_depth_ here. translate_block calls
    // make_writable() at entry and make_executable() at exit, keeping
    // the buffer RX whenever JIT code might run.
    std::unique_lock<std::shared_mutex> lock(blocks_mutex_);
    blocks_.clear();
    tier2_block_limits_.clear();
    code_index_.clear();
    back_refs_.clear();
    invalidate_dispatch_cache();
    tls_hot_pc_counts_.clear();  // clear hotness tracker (thread-local)
    // Retain code bytes; live function pointers must remain valid.
}
} // namespace arm64emu
