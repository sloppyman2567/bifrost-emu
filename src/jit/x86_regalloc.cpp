// jit/x86_regalloc.cpp — virtual-register allocator + spill/evict/flush.
//
// Maps IR vregs to host x86 registers. Each vreg has a "home" x86 reg
// (or -1 if spilled to stack). The allocator tracks which vreg owns each
// x86 reg. When a vreg is needed, its home reg is used directly (no
// load/store). When a reg is needed for a new vreg, the old owner is
// spilled if dirty.
//
// Methods implemented here:
//   vreg_stack_slot      — allocate a stack slot for a spilled vreg
//   evict_vreg           — spill a vreg from its host reg to stack
//   alloc_reg            — pick a free host reg (no preferred)
//   alloc_reg_for        — pick a host reg for vreg v, evicting old occupant
//   ensure_vreg          — make sure vreg v is in a host reg, load if needed
//   set_vreg_reg         — bind vreg v to a specific host reg
//   kill_vreg            — drop a vreg from its host reg (no spill)
//   flush_all_vregs      — write back all dirty vregs to cpu.regs[]/stack
//   invalidate_all_vregs — drop all vreg→host mappings (after a C call)
//   drop_vreg            — safe drop: evict if dirty, then clear mapping
//   clobber_host_reg     — evict occupant of a host reg if dirty
//   force_vreg_to_reg    — move/load vreg v into a specific host reg
//   force_two_vregs_to   — same for two vregs (handles aliasing)
#include "jit/frostjit.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>   // getenv, abort
#include <cstdint>   // UINT32_MAX
namespace arm64emu {
static regalloc_stats_t g_rs;
void regalloc_stats_reset() { g_rs = regalloc_stats_t{}; }
const regalloc_stats_t& regalloc_stats_get() { return g_rs; }
// Bounds-check helper: ensures vreg index is within the fixed-size arrays.
// If a block exceeds MAX_VREGS, we'd have a buffer overflow. This check
// catches it at the earliest point (vreg allocation) instead of silently
// corrupting memory.
//
// Active in debug builds (NDEBUG not defined) OR when BIFROST_REGALLOC_CHECK
// is set. In release builds without the env var, the check is skipped for
// performance (the typical max_vreg_ is 33-250, well within the 4096 limit).
static inline bool regalloc_check_enabled() {
#ifndef NDEBUG
    return true;
#else
    static bool enabled = (getenv("BIFROST_REGALLOC_CHECK") != nullptr);
    return enabled;
#endif
}
static inline void check_vreg_bounds(int v) {
    if (!regalloc_check_enabled()) return;
    if (v < 0 || v >= 4096) {
        fprintf(stderr, "[JIT REGALLOC BUG] vreg %d out of bounds (max 4096). "
                "Block too long? Please report this.\n", v);
        // In debug, abort so the bug is caught immediately. In release with
        // the env var, just log (don't crash the user's program).
#ifndef NDEBUG
        abort();
#endif
    }
}
// Verify the dirty_host_regs_ invariant.
// Bit r is set iff reg_vreg_[r] >= 0 && vreg_dirty_[reg_vreg_[r]].
// Called at block boundaries to catch maintenance bugs.
bool FrostJIT::verify_dirty_host_regs_() const {
    if (!regalloc_check_enabled()) return true;
    for (int r = 0; r < NUM_HOST_REGS; r++) {
        int v = reg_vreg_[r];
        bool bit_set = (dirty_host_regs_ >> r) & 1;
        bool should_set = (v >= 0 && v < 4096 && vreg_dirty_[v]);
        if (bit_set != should_set) {
            fprintf(stderr, "[JIT REGALLOC BUG] dirty_host_regs_ bit %d mismatch: "
                    "bit=%d expected=%d (reg_vreg_[%d]=%d, vreg_dirty_[%d]=%d)\n",
                    r, bit_set, should_set, r, v, v, (v >= 0 && v < 4096) ? vreg_dirty_[v] : 0);
            return false;
        }
    }
    return true;
}
int32_t FrostJIT::vreg_stack_slot(int v) {
    check_vreg_bounds(v);
    if (vreg_slot_[v] != 0) return vreg_slot_[v];
    num_stack_slots_++;
    vreg_slot_[v] = -8 * num_stack_slots_;
    return vreg_slot_[v];
}
// FP register index validation. FP ops use inst.dest/src1/src2 as FP
// register indices (0-31). Values > 31 would cause out-of-bounds writes
// to the CPU struct (v_lo[32+] or v_hi[32+] = past the array).
void FrostJIT::check_fp_reg_index(int idx, const char* context) const {
    if (!regalloc_check_enabled()) return;
    if (idx < 0 || idx > 31) {
        fprintf(stderr, "[JIT CODEGEN BUG] FP register index %d out of bounds "
                "(must be 0-31) in %s. Decoder or IR translator bug?\n",
                idx, context);
#ifndef NDEBUG
        abort();
#endif
    }
}
// Spill a vreg from its x86 reg back to its home (cpu.regs[] or stack).
void FrostJIT::evict_vreg(int v) {
    int r = vreg_home_[v];
    if (r < 0) return;
    g_rs.evicts++;
    if (vreg_dirty_[v]) {
        if (v <= 31) {
            emit_store_arm(v, r);  // write back to cpu.regs[]/sp
            g_rs.spill_arm++;
        } else {
            int32_t off = vreg_stack_slot(v);
            emit_store(RBP, off, r);
            g_rs.spill_stack++;
        }
    }
    vreg_home_[v] = -1;
    reg_vreg_[r] = -1;
    vreg_dirty_[v] = false;
    vreg_last_use_[v] = 0;  // clear LRU timestamp
    dirty_host_regs_ &= ~(1u << r);  // vreg no longer dirty in r
}
// Drop a vreg's cache mapping WITHOUT spilling.
//
// This is the "safe drop" helper: if the vreg is dirty (its cached value
// differs from memory), we EVICT it first (write back to cpu.regs[] or
// stack slot) so the value is preserved. Only then do we clear the
// mapping.
//
// Use this instead of the raw pattern:
//     vreg_home_[v] = -1;
//     vreg_dirty_[v] = false;
// which silently loses dirty values. That pattern was the root cause of
// the jit_simd crash — REV64 and CLZ both dropped a dirty
// src1 vreg without spilling, then a later reader reloaded from an
// uninitialized stack slot.
//
// If you INTEND to drop a dirty value (because the value is genuinely
// dead — e.g. the register was overwritten by a computation whose result
// you're keeping), use kill_vreg(v) instead, which does NOT evict.
//
// In debug builds (NDEBUG not defined), this function asserts that the
// regalloc state is consistent: reg_vreg_[vreg_home_[v]] == v.
void FrostJIT::drop_vreg(int v) {
    int r = vreg_home_[v];
    if (r < 0) {
        // Not cached — nothing to do.
        vreg_dirty_[v] = false;
        return;
    }
#ifndef NDEBUG
    // Consistency check: the reverse mapping must agree.
    if (reg_vreg_[r] != v) {
        fprintf(stderr, "[JIT REGALLOC BUG] drop_vreg(%d): reg_vreg_[%d]=%d (expected %d) — "
                "cache state corrupted. This is a bug in the JIT codegen; please report it.\n",
                v, r, reg_vreg_[r], v);
    }
#endif
    if (vreg_dirty_[v]) {
        // Value is dirty — preserve it by spilling to memory first.
        if (v <= 31) {
            emit_store_arm(v, r);
            g_rs.spill_arm++;
        } else {
            int32_t off = vreg_stack_slot(v);
            emit_store(RBP, off, r);
            g_rs.spill_stack++;
        }
    }
    vreg_home_[v] = -1;
    reg_vreg_[r] = -1;
    vreg_dirty_[v] = false;
    vreg_last_use_[v] = 0;  // clear LRU timestamp
    dirty_host_regs_ &= ~(1u << r);  // vreg no longer dirty in r
}
// Evict the occupant of `host_reg` if dirty, then clear the mapping.
// Use this BEFORE clobbering `host_reg` with a computation that doesn't
// preserve the old value (e.g. emit_mov_imm64(RAX, ...) in FP_MOVI).
void FrostJIT::clobber_host_reg(int host_reg) {
    int v = reg_vreg_[host_reg];
    if (v < 0) return;
    if (vreg_dirty_[v]) {
        // Preserve the dirty value by spilling to memory.
        if (v <= 31) {
            emit_store_arm(v, host_reg);
            g_rs.spill_arm++;
        } else {
            int32_t off = vreg_stack_slot(v);
            emit_store(RBP, off, host_reg);
            g_rs.spill_stack++;
        }
    }
    vreg_home_[v] = -1;
    reg_vreg_[host_reg] = -1;
    vreg_dirty_[v] = false;
    vreg_last_use_[v] = 0;  // clear LRU timestamp
    dirty_host_regs_ &= ~(1u << host_reg);  // host_reg no longer holds dirty vreg
}
int FrostJIT::alloc_reg(int preferred) {
    // Try preferred first.
    if (preferred >= 0 && reg_vreg_[preferred] == -1) {
        return preferred;
    }
    // Try each alloc reg in order.
    for (int i = 0; i < NUM_ALLOC_REGS; i++) {
        int r = ALLOC_REGS[i];
        if (reg_vreg_[r] == -1) return r;
    }
    // All regs taken — evict the LRU vreg (true least-recently-used, not FIFO).
    // Scan all alloc regs and pick the one whose cached vreg has the smallest
    // vreg_last_use_ timestamp. This avoids evicting a hot vreg just because
    // it was allocated first (the old FIFO behavior that always evicted
    // ALLOC_REGS[0] = RAX).
    int best_r = ALLOC_REGS[0];
    uint32_t best_ts = vreg_last_use_[reg_vreg_[best_r]];
    for (int i = 1; i < NUM_ALLOC_REGS; i++) {
        int r = ALLOC_REGS[i];
        int v = reg_vreg_[r];
        if (v >= 0 && vreg_last_use_[v] < best_ts) {
            best_ts = vreg_last_use_[v];
            best_r = r;
        }
    }
    int r = best_r;
    int v = reg_vreg_[r];
    if (v >= 0) evict_vreg(v);
    return r;
}
// Allocate a host reg excluding `excl1` and `excl2`. Used by ALU codegen
// to place `dest` in a reg that doesn't collide with src1/src2's host regs,
// so we can compute dest = src1 op src2 without spilling the operands.
int FrostJIT::alloc_reg_excluding(int excl1, int excl2) {
    // First pass: look for a free reg (skipping excluded ones).
    for (int i = 0; i < NUM_ALLOC_REGS; i++) {
        int r = ALLOC_REGS[i];
        if (r == excl1 || r == excl2) continue;
        if (reg_vreg_[r] == -1) return r;
    }
    // No free reg — evict a non-excluded reg using true LRU.
    // Scan all non-excluded alloc regs and pick the one with the oldest
    // vreg_last_use_ timestamp.
    int best_r = -1;
    uint32_t best_ts = UINT32_MAX;
    for (int i = 0; i < NUM_ALLOC_REGS; i++) {
        int r = ALLOC_REGS[i];
        if (r == excl1 || r == excl2) continue;
        int v = reg_vreg_[r];
        if (v >= 0 && vreg_last_use_[v] < best_ts) {
            best_ts = vreg_last_use_[v];
            best_r = r;
        }
    }
    if (best_r >= 0) {
        evict_vreg(reg_vreg_[best_r]);
        return best_r;
    }
    // All alloc regs are excluded — shouldn't happen (we have 9 alloc regs
    // and only exclude at most 2). Fall back to alloc_reg.
    return alloc_reg();
}
// Ensure vreg v is in an x86 reg. Returns the reg.
// `preferred` is a HINT for newly loaded vregs only — if v is already
// cached, we return its current reg WITHOUT moving (avoids overhead).
int FrostJIT::ensure_vreg(int v, int preferred) {
    if (v > max_vreg_) max_vreg_ = v;
    // Already cached? Just return it — no moving.
    if (vreg_home_[v] >= 0) return vreg_home_[v];
    // Need to load. Try preferred first, then any free reg.
    int r = alloc_reg(preferred);
    // Load v into r.
    if (v <= 31) {
        emit_load_arm(r, v);
        g_rs.reload_arm++;
    } else {
        int32_t off = vreg_stack_slot(v);
        emit_load(r, RBP, off);
        g_rs.reload_stack++;
    }
    vreg_home_[v] = r;
    reg_vreg_[r] = v;
    vreg_dirty_[v] = false;
    vreg_last_use_[v] = ++regalloc_lru_counter_;  // mark as recently used
    return r;
}
// Record that vreg v is now in reg r (e.g., after a computation).
// The old occupant of reg r is KILLED (not evicted) — its value was
// already overwritten by the computation, so we must NOT write it back.
// If the old vreg was dirty, its modified value is lost. Callers must
// ensure dirty vregs are evicted BEFORE overwriting the register.
void FrostJIT::set_vreg_reg(int v, int r) {
    if (v > max_vreg_) max_vreg_ = v;
    // If v was in a different reg, drop that mapping (v is moving).
    if (vreg_home_[v] >= 0 && vreg_home_[v] != r) {
        // Clear dirty bit for the old reg if v was dirty there.
        if (vreg_dirty_[v]) dirty_host_regs_ &= ~(1u << vreg_home_[v]);
        reg_vreg_[vreg_home_[v]] = -1;
    }
    // If r held a different vreg, KILL it — the computation already
    // overwrote the register's content.
    int old_v = reg_vreg_[r];
    if (old_v >= 0 && old_v != v) {
        vreg_home_[old_v] = -1;
        vreg_dirty_[old_v] = false;
        // old_v was either dirty (now lost) or clean — either way, r no
        // longer holds old_v's value. Clear the bit; we'll set it below
        // if v becomes dirty in r.
        dirty_host_regs_ &= ~(1u << r);
    }
    vreg_home_[v] = r;
    reg_vreg_[r] = v;
    vreg_dirty_[v] = true;
    vreg_last_use_[v] = ++regalloc_lru_counter_;  // mark as recently used
    dirty_host_regs_ |= (1u << r);  // v is now dirty in r
}
// Allocate reg r for vreg v, evicting the current occupant FIRST (before
// any computation overwrites the register). Use this instead of
// set_vreg_reg when you need to preserve the old occupant's value.
int FrostJIT::alloc_reg_for(int v, int preferred) {
    if (v > max_vreg_) max_vreg_ = v;
    // If v is already in a reg, use it.
    if (vreg_home_[v] >= 0) return vreg_home_[v];
    // Allocate a reg, evicting if needed.
    int r = alloc_reg(preferred);
    // Evict the current occupant BEFORE any computation.
    int old_v = reg_vreg_[r];
    if (old_v >= 0 && old_v != v) {
        evict_vreg(old_v);
    }
    // Record v in r (no value loaded — caller will set it via computation).
    vreg_home_[v] = r;
    reg_vreg_[r] = v;
    vreg_dirty_[v] = true;
    vreg_last_use_[v] = ++regalloc_lru_counter_;  // mark as recently used
    dirty_host_regs_ |= (1u << r);  // v is now dirty in r
    return r;
}
// Drop a vreg's register mapping (value is dead / will be overwritten).
void FrostJIT::kill_vreg(int v) {
    int r = vreg_home_[v];
    if (r >= 0) {
        reg_vreg_[r] = -1;
        vreg_home_[v] = -1;
        vreg_dirty_[v] = false;
        vreg_last_use_[v] = 0;  // clear LRU timestamp
        dirty_host_regs_ &= ~(1u << r);  // r no longer holds v (or any dirty vreg)
    }
    vreg_dirty_[v] = false;
}
// Spill all dirty vregs to their home (before CALL_INTERP/SVC/branch).
// walks the dirty_host_regs_ bitmask — O(popcount) instead
// of O(max_vreg_). For typical blocks (max_vreg_ ≈ 100) this is ~10x
// faster; for FP-heavy blocks (max_vreg_ ≈ 256+) it's 30x+ faster.
void FrostJIT::flush_all_vregs() {
    uint16_t m = dirty_host_regs_;
    while (m) {
        int r = __builtin_ctz(m);
        m &= m - 1;  // clear lowest set bit
        int v = reg_vreg_[r];
        if (v >= 0 && vreg_dirty_[v]) {
            evict_vreg(v);
        }
    }
}
// ── Targeted flush/invalidate (v1.4.0-beta.2) ────────────────────────
// Walk only the host regs whose bits are set in `mask`, spilling any
// dirty vreg cached there. This is the heart of the flush-penalty
// reduction: FP JIT ops can flush just RAX/RCX/RDX in ~3 iterations
// instead of scanning all max_vreg_ vreg entries.
void FrostJIT::flush_dirty_host_regs(uint16_t mask) {
    uint16_t m = dirty_host_regs_ & mask;
    while (m) {
        int r = __builtin_ctz(m);
        m &= m - 1;
        int v = reg_vreg_[r];
        if (v >= 0 && vreg_dirty_[v]) {
            evict_vreg(v);
        }
    }
}
// Drop cache mappings for host regs in `mask` (no spill — caller must
// have already flushed if any were dirty). Companion to above.
void FrostJIT::invalidate_host_regs(uint16_t mask) {
    uint16_t m = mask;
    while (m) {
        int r = __builtin_ctz(m);
        m &= m - 1;
        int v = reg_vreg_[r];
        if (v >= 0) {
            vreg_home_[v] = -1;
            reg_vreg_[r] = -1;
            vreg_dirty_[v] = false;
        }
        dirty_host_regs_ &= ~(1u << r);
    }
}
// Spill scratch vregs (v > 31) in `mask` only when DIRTY. Every caller
// runs flush_dirty_host_regs on the same mask first, which evicts (spills +
// unmaps) all dirty vregs — so at flush_scratch time any remaining cached
// scratch is CLEAN, and the dirty-flag invariant (dirty=false ⇒ the stack
// slot holds the current value) makes the store redundant. The
// `vreg_dirty_[v]` guard keeps the defensive standalone-call behavior the
// header comment describes: a scratch whose value lives ONLY in the host reg
// still gets spilled.
void FrostJIT::flush_scratch_host_regs(uint16_t mask) {
    uint16_t m = mask;
    while (m) {
        int r = __builtin_ctz(m);
        m &= m - 1;
        int v = reg_vreg_[r];
        if (v >= 0 && v > 31) {
            if (vreg_dirty_[v]) {
                // Dirty scratch: spill to its stack slot.
                int32_t off = vreg_stack_slot(v);
                emit_store(RBP, off, r);
                g_rs.spill_stack++;
            }
            // Mark as non-dirty (we just wrote it to its home — or it
            // already was).
            vreg_dirty_[v] = false;
            dirty_host_regs_ &= ~(1u << r);
        }
    }
}
// ── vreg_last_use_this_op ──────────────────────────────────────────────
// True if scratch vreg `v` has its LAST use at the current op (i.e. it is
// dead once the current op has been emitted) and is NOT the dest of the
// current op. The current op index is cur_op_index_, set in
// translate_block's compile loop. Used by the flush-skip fast paths in
// UBFM/SBFM/LOAD_MEM/STORE_MEM: a dead scratch vreg already cached in
// the destination host reg needs neither a spill (no later reader) nor a
// reload (the value is already there).
bool FrostJIT::vreg_last_use_this_op(int v) const {
    if (cur_op_index_ >= kills_per_op_.size()) return false;
    const auto& kills = kills_per_op_[cur_op_index_];
    for (uint16_t k : kills) {
        if (k == v) return true;
    }
    return false;
}
bool FrostJIT::vreg_fast_keep_candidate(int v, int reg, int dest_vreg) const {
    if (v <= 32 || v >= 4096 || v == dest_vreg) return false;
    if (vreg_home_[v] != reg) return false;
    return vreg_last_use_this_op(v);
}
// ── load_vreg_to_reg_fast ──────────────────────────────────────────────
// Load vreg `v` into host reg `dst` before an op that clobbers the regs
// in `clobber_mask` (dst must be a member of the mask). Avoids the
// flush→reload sandwich (`mov dst,slot; mov slot,dst`) when the previous
// op left `v` cached in `dst`:
//
//   Tier 1 (always safe): v is already cached in `dst`. The flush never
//   modifies the register (it only writes to memory), so `dst` still
//   physically holds v's value after the flush+invalidate. Skip the
//   redundant reload. The mapping is dropped (v reloads from memory if
//   a later op needs it).
//   Tier 2 (dead scratch only): additionally, if v is a scratch vreg
//   (v > 32) that dies at this op, skip the flush of `dst` entirely and
//   keep v mapped dirty in `dst` — the op consumes it and the caller's
//   trailing set_vreg_reg(dest, dst) (UBFM/SBFM) or explicit
//   kill_vreg(v) (LOAD_MEM) drops the mapping.
//
// Returns the set of host regs in `clobber_mask` that were kept live
// (bit set = that reg's scratch vreg stayed cached there, Tier 2). Pass
// `clobber_mask & ~kept` to a subsequent call (STORE_MEM loads both
// operands) so an earlier kept reg isn't flushed by the second call.
uint16_t FrostJIT::load_vreg_to_reg_fast(int dst, int v, int dest_vreg,
                                         uint16_t clobber_mask) {
    // Capture cache state BEFORE any flush (flush+invalidate drops mappings).
    bool in_dst = (v <= max_vreg_ && vreg_home_[v] == dst);
    int home = (v <= max_vreg_) ? vreg_home_[v] : -1;
    // Tier 2: dead scratch vreg already cached in dst. No spill, no
    // invalidate of dst, no reload — keep it mapped for the op to consume.
    if (v > 32 && v < 4096 && v != dest_vreg && in_dst &&
        vreg_last_use_this_op(v)) {
        uint16_t flush_mask = clobber_mask & ~(1u << dst);
        flush_dirty_host_regs(flush_mask);
        flush_scratch_host_regs(flush_mask);
        invalidate_host_regs(flush_mask);
        vreg_last_use_[v] = ++regalloc_lru_counter_;  // mark as recently used
        return (1u << dst);
    }
    // Tier 1.5: v is cached in ANOTHER reg that the op will clobber. Copy
    // it to dst first, then flush+invalidate (the flush preserves the value
    // for later readers via its stack slot), skip the reload. Common case:
    // ADD/SHL leaves a scratch vreg in RCX, then LOAD_MEM consumes it.
    //
    // CRITICAL: clobber_host_reg(dst) BEFORE the mov. If dst was still
    // mapped to a DIFFERENT dirty vreg, the mov destroys that cached value
    // and the subsequent flush_dirty_host_regs writes the NEW value into the
    // old vreg's stack slot (value corruption). This is the same class of
    // bug as the SIMD DUP broadcast: drop the mapping (spilling if dirty)
    // before overwriting the register.
    if (home >= 0 && home != dst && (clobber_mask & (1u << home))) {
        clobber_host_reg(dst);
        emit_mov_reg(dst, home);
        flush_dirty_host_regs(clobber_mask);
        flush_scratch_host_regs(clobber_mask);
        invalidate_host_regs(clobber_mask);
        vreg_last_use_[v] = ++regalloc_lru_counter_;
        return 0;
    }
    // Flush+invalidate everything in the clobber mask.
    flush_dirty_host_regs(clobber_mask);
    flush_scratch_host_regs(clobber_mask);
    invalidate_host_regs(clobber_mask);
    // Tier 1: v was already in dst — the flush preserved the value in
    // memory and dst still physically holds it, so skip the redundant reload.
    if (in_dst) {
        return 0;  // no live mapping; dst holds the value for the op to use
    }
    load_vreg_to_reg(dst, v);
    return 0;
}
// ── Codegen helpers (reduce boilerplate in compile_ir_inst) ────────────
// These wrap the "load vreg to host reg" / "store host reg to vreg"
// patterns. load_vreg_to_reg is cache-aware: if v is already cached in
// a host reg, it emits a mov from that reg (preserving the dirty value)
// instead of loading stale data from memory. store_reg_to_vreg writes
// to memory and kills any stale cache mapping for v.
void FrostJIT::load_vreg_to_reg(int dst, int v) {
    // If v is cached in a host reg, mov from there — the cached value
    // may be dirty and not yet written to memory.
    if (v <= max_vreg_ && vreg_home_[v] >= 0) {
        int src = vreg_home_[v];
        if (src != dst) emit_mov_reg(dst, src);
        vreg_last_use_[v] = ++regalloc_lru_counter_;  // mark as recently used
        return;
    }
    // Not cached — load from memory (cpu.regs[] or stack slot).
    if (v <= 31) {
        emit_load_arm(dst, v);
        g_rs.reload_arm++;
    } else {
        int32_t off = vreg_stack_slot(v);
        emit_load(dst, RBP, off);
        g_rs.reload_stack++;
    }
}
void FrostJIT::store_reg_to_vreg(int v, int src) {
    // Write to memory (cpu.regs[] or stack slot).
    if (v <= 31) {
        emit_store_arm(v, src);
    } else {
        int32_t off = vreg_stack_slot(v);
        emit_store(RBP, off, src);
    }
    // Kill any stale cache mapping for v — memory now has the new value,
    // but a cached entry would still hold the old one.
    if (v <= max_vreg_ && vreg_home_[v] >= 0) {
        int r = vreg_home_[v];
        reg_vreg_[r] = -1;
        vreg_home_[v] = -1;
        vreg_dirty_[v] = false;
        vreg_last_use_[v] = 0;  // clear LRU timestamp
        dirty_host_regs_ &= ~(1u << r);
    }
}
// Drop all cached vreg→reg mappings WITHOUT spilling.
// Used after operations that clobber all caller-saved regs (C calls).
// Assumes flush_all_vregs was called BEFORE the clobbering operation,
// so all dirty values were already written back. This just drops the
// stale reg→vreg associations.
void FrostJIT::invalidate_all_vregs() {
    for (int v = 0; v <= max_vreg_; v++) {
        int r = vreg_home_[v];
        if (r >= 0) {
            reg_vreg_[r] = -1;
            vreg_home_[v] = -1;
            vreg_dirty_[v] = false;
            vreg_last_use_[v] = 0;  // clear LRU timestamp
        }
    }
    dirty_host_regs_ = 0;
    flags_in_host_ = false;
}
// ── force_vreg_to_reg ──────────────────────────────────────────────────
// Force vreg `v` to live in host register `host_reg` (MOVE semantics).
//
// This replaces the manually-inlined "evict occupant of host_reg, then
// either move v from its current home or load v from memory" boilerplate
// that was duplicated across SHL/SHR/SAR/ROR, ADDS/SUBS, and ADCS/SBCS
// cases (each ~50 lines). The caller now writes:
//
//     force_vreg_to_reg(inst.src1, RAX);
//     force_vreg_to_reg(inst.src2, RCX);
//
// instead of inlining the eviction + move/load logic each time.
//
// After this call:
//   vreg_home_[v]   == host_reg
//   reg_vreg_[host_reg] == v
//
// Any previous occupant of `host_reg` (other than `v` itself) is evicted
// (spilled if dirty). If `v` was previously in another reg, that reg's
// mapping is cleared (the value moves to `host_reg`).
void FrostJIT::force_vreg_to_reg(int v, int host_reg) {
    if (v > max_vreg_) max_vreg_ = v;
    // Step 1: evict whatever is in host_reg (unless it's already v).
    int cur = reg_vreg_[host_reg];
    if (cur >= 0 && cur != v) {
        evict_vreg(cur);
    }
    // Step 2: place v in host_reg.
    int home = vreg_home_[v];
    if (home == host_reg) {
        return;  // already there
    }
    if (home >= 0) {
        // v is in another reg — move it (clear old mapping).
        // Note: we preserve v's dirty bit (it stays dirty in the new home).
        bool was_dirty = vreg_dirty_[v];
        emit_mov_reg(host_reg, home);
        reg_vreg_[home] = -1;
        if (was_dirty) dirty_host_regs_ &= ~(1u << home);
    } else {
        // v is spilled — load from memory (not dirty after load).
        if (v <= 31) {
            emit_load_arm(host_reg, v);
        } else {
            int32_t off = vreg_stack_slot(v);
            emit_load(host_reg, RBP, off);
        }
    }
    vreg_home_[v] = host_reg;
    reg_vreg_[host_reg] = v;
    // Dirty bit is preserved: if v was dirty before, it's still dirty
    // (we just moved its value, not written it back).
    if (vreg_dirty_[v]) dirty_host_regs_ |= (1u << host_reg);
    vreg_last_use_[v] = ++regalloc_lru_counter_;  // mark as recently used
}
// ── force_two_vregs_to ─────────────────────────────────────────────────
// Force two vregs into two specific host registers in one call.
//
// Handles the aliasing case where src1 == src2 (or src2 was originally
// cached in host_reg1) by COPYING src2 to host_reg2 instead of moving,
// so src1's mapping in host_reg1 is preserved.
//
// After this call:
//   vreg_home_[src1] == host_reg1,  reg_vreg_[host_reg1] == src1
//   vreg_home_[src2] == host_reg2,  reg_vreg_[host_reg2] == src2
//
// When src1 == src2, both host_reg1 and host_reg2 hold the same value;
// reg_vreg_[host_reg1] stays as src1 (aliasing), and vreg_home_[src1]
// is set to host_reg2 (the most recent force wins).
void FrostJIT::force_two_vregs_to(int src1, int host_reg1,
                                  int src2, int host_reg2) {
    // Force src1 into host_reg1 (MOVE semantics).
    force_vreg_to_reg(src1, host_reg1);
    // Force src2 into host_reg2. Evict host_reg2's current occupant
    // if it's not src2. (If src1 == src2 and src1 is now in host_reg1,
    // the eviction of host_reg2 won't touch host_reg1.)
    if (reg_vreg_[host_reg2] >= 0 && reg_vreg_[host_reg2] != src2) {
        evict_vreg(reg_vreg_[host_reg2]);
    }
    int home2 = vreg_home_[src2];
    if (home2 == host_reg2) {
        return;  // already there
    }
    bool was_dirty2 = vreg_dirty_[src2];
    if (home2 == host_reg1) {
        // src2 is in host_reg1 (either because src1 == src2, or src2
        // was independently cached there). COPY — don't clear
        // host_reg1's mapping, because src1 needs to stay there.
        emit_mov_reg(host_reg2, host_reg1);
    } else if (home2 >= 0) {
        // src2 is in some other reg — move it (clear old mapping).
        emit_mov_reg(host_reg2, home2);
        reg_vreg_[home2] = -1;
        if (was_dirty2) dirty_host_regs_ &= ~(1u << home2);
    } else {
        // src2 is spilled — load from memory.
        if (src2 <= 31) {
            emit_load_arm(host_reg2, src2);
        } else {
            int32_t off = vreg_stack_slot(src2);
            emit_load(host_reg2, RBP, off);
        }
    }
    vreg_home_[src2] = host_reg2;
    reg_vreg_[host_reg2] = src2;
    // Preserve dirty bit.
    if (was_dirty2) dirty_host_regs_ |= (1u << host_reg2);
    else            dirty_host_regs_ &= ~(1u << host_reg2);
    vreg_last_use_[src2] = ++regalloc_lru_counter_;  // mark as recently used
}
// ── emit_fmov_helper ───────────────────────────────────────────────────
// Unified FMOV codegen for all four GPR↔FP register moves:
//   dir=0, fp_field=0: FMOV_G2F   — v_lo[idx] = src1; v_hi[idx] = 0
//   dir=0, fp_field=1: FMOV_G2FHI — v_hi[idx] = src1
//   dir=1, fp_field=0: FMOV_F2G   — dest = v_lo[idx]
//   dir=1, fp_field=1: FMOV_FHI2G — dest = v_hi[idx]
//
// G→F path uses RCX as scratch for the zero store
// (v_hi) so src1 stays cached in RAX. The F→G path loads the FP slot
// into a fresh vreg via alloc_reg + emit_load + set_vreg_reg.
} // namespace arm64emu
