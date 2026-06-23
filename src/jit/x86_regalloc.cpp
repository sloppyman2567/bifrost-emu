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
//   flush_caller_saved_vregs — same, but only caller-saved (R12/R13/R15 stay)
//   invalidate_all_vregs — drop all vreg→host mappings (after a C call)
//   invalidate_caller_saved_vregs — same, but only caller-saved
//   drop_vreg            — safe drop: evict if dirty, then clear mapping
//   clobber_host_reg     — evict occupant of a host reg if dirty
//   force_vreg_to_reg    — move/load vreg v into a specific host reg
//   force_two_vregs_to   — same for two vregs (handles aliasing)
#include "jit/frostjit.hpp"

#include <cstdio>
#include <cstring>

namespace arm64emu {

// Bounds-check helper: ensures vreg index is within the fixed-size arrays.
// If a block exceeds MAX_VREGS, we'd have a buffer overflow. This assert
// catches it at the earliest point (vreg allocation) instead of silently
// corrupting memory.
static inline void check_vreg_bounds(int v) {
#ifndef NDEBUG
    if (v < 0 || v >= 4096) {
        fprintf(stderr, "[JIT REGALLOC BUG] vreg %d out of bounds (max 4096). "
                "Block too long? Please report this.\n", v);
    }
#endif
}

int32_t FrostJIT::vreg_stack_slot(int v) {
    check_vreg_bounds(v);
    if (vreg_slot_[v] != 0) return vreg_slot_[v];
    num_stack_slots_++;
    vreg_slot_[v] = -8 * num_stack_slots_;
    return vreg_slot_[v];
}

// Spill a vreg from its x86 reg back to its home (cpu.regs[] or stack).
void FrostJIT::evict_vreg(int v) {
    int r = vreg_home_[v];
    if (r < 0) return;
    if (vreg_dirty_[v]) {
        if (v <= 31) {
            emit_store_arm(v, r);  // write back to cpu.regs[]/sp
        } else {
            int32_t off = vreg_stack_slot(v);
            emit_store(RBP, off, r);
        }
    }
    vreg_home_[v] = -1;
    reg_vreg_[r] = -1;
    vreg_dirty_[v] = false;
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
// the v1.4.0-beta.1 jit_simd crash — REV64 and CLZ both dropped a dirty
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
        } else {
            int32_t off = vreg_stack_slot(v);
            emit_store(RBP, off, r);
        }
    }
    vreg_home_[v] = -1;
    reg_vreg_[r] = -1;
    vreg_dirty_[v] = false;
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
        } else {
            int32_t off = vreg_stack_slot(v);
            emit_store(RBP, off, host_reg);
        }
    }
    vreg_home_[v] = -1;
    reg_vreg_[host_reg] = -1;
    vreg_dirty_[v] = false;
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
    // All regs taken — evict the first one (simple LRU-ish).
    int r = ALLOC_REGS[0];
    int v = reg_vreg_[r];
    if (v >= 0) evict_vreg(v);
    return r;
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
    } else {
        int32_t off = vreg_stack_slot(v);
        emit_load(r, RBP, off);
    }
    vreg_home_[v] = r;
    reg_vreg_[r] = v;
    vreg_dirty_[v] = false;
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
        reg_vreg_[vreg_home_[v]] = -1;
    }
    // If r held a different vreg, KILL it — the computation already
    // overwrote the register's content.
    int old_v = reg_vreg_[r];
    if (old_v >= 0 && old_v != v) {
        vreg_home_[old_v] = -1;
        vreg_dirty_[old_v] = false;
    }
    vreg_home_[v] = r;
    reg_vreg_[r] = v;
    vreg_dirty_[v] = true;
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
    return r;
}

// Drop a vreg's register mapping (value is dead / will be overwritten).
void FrostJIT::kill_vreg(int v) {
    int r = vreg_home_[v];
    if (r >= 0) {
        reg_vreg_[r] = -1;
        vreg_home_[v] = -1;
    }
    vreg_dirty_[v] = false;
}

// Spill all dirty vregs to their home (before CALL_INTERP/SVC/branch).
void FrostJIT::flush_all_vregs() {
    for (int v = 0; v <= max_vreg_; v++) {
        if (vreg_home_[v] >= 0 && vreg_dirty_[v]) {
            evict_vreg(v);
        }
    }
}

// (v1.4.0-alpha.5): flush only caller-saved dirty vregs. Callee-saved
// regs (R12/R13/R15) are preserved by C calls, so vregs cached there
// don't need to be spilled around CALL_INTERP / memory slow paths.
void FrostJIT::flush_caller_saved_vregs() {
    for (int v = 0; v <= max_vreg_; v++) {
        int r = vreg_home_[v];
        if (r >= 0 && vreg_dirty_[v] && is_caller_saved(r)) {
            evict_vreg(v);
        }
    }
}

// ── Codegen helpers (reduce boilerplate in compile_ir_inst) ────────────
// These wrap the "load vreg to host reg" / "store host reg to vreg"
// patterns that were duplicated across LOAD_MEM, STORE_MEM, FP_MOVI,
// SIMD_DUP, SIMD_LDST, etc. Each replaces a 2-line if/else with one call.

void FrostJIT::load_vreg_to_reg(int dst, int v) {
    if (v <= 31) {
        emit_load_arm(dst, v);
    } else {
        int32_t off = vreg_stack_slot(v);
        emit_load(dst, RBP, off);
    }
}

void FrostJIT::store_reg_to_vreg(int v, int src) {
    if (v <= 31) {
        emit_store_arm(v, src);
    } else {
        int32_t off = vreg_stack_slot(v);
        emit_store(RBP, off, src);
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
        }
    }
    flags_in_host_ = false;
}

// (v1.4.0-alpha.5): invalidate only caller-saved cache mappings.
// Callee-saved vregs (in R12/R13/R15) are still valid after a C call.
void FrostJIT::invalidate_caller_saved_vregs() {
    for (int v = 0; v <= max_vreg_; v++) {
        int r = vreg_home_[v];
        if (r >= 0 && is_caller_saved(r)) {
            reg_vreg_[r] = -1;
            vreg_home_[v] = -1;
            vreg_dirty_[v] = false;
        }
    }
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
        emit_mov_reg(host_reg, home);
        reg_vreg_[home] = -1;
    } else {
        // v is spilled — load from memory.
        if (v <= 31) {
            emit_load_arm(host_reg, v);
        } else {
            int32_t off = vreg_stack_slot(v);
            emit_load(host_reg, RBP, off);
        }
    }
    vreg_home_[v] = host_reg;
    reg_vreg_[host_reg] = v;
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
    if (home2 == host_reg1) {
        // src2 is in host_reg1 (either because src1 == src2, or src2
        // was independently cached there). COPY — don't clear
        // host_reg1's mapping, because src1 needs to stay there.
        emit_mov_reg(host_reg2, host_reg1);
    } else if (home2 >= 0) {
        // src2 is in some other reg — move it (clear old mapping).
        emit_mov_reg(host_reg2, home2);
        reg_vreg_[home2] = -1;
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
}

// ── emit_fmov_helper ───────────────────────────────────────────────────
// Unified FMOV codegen for all four GPR↔FP register moves:
//   dir=0, fp_field=0: FMOV_G2F   — v_lo[idx] = src1; v_hi[idx] = 0
//   dir=0, fp_field=1: FMOV_G2FHI — v_hi[idx] = src1
//   dir=1, fp_field=0: FMOV_F2G   — dest = v_lo[idx]
//   dir=1, fp_field=1: FMOV_FHI2G — dest = v_hi[idx]
//
// (v1.4.0-beta.1): G→F path uses RCX as scratch for the zero store
// (v_hi) so src1 stays cached in RAX. The F→G path loads the FP slot
// into a fresh vreg via alloc_reg + emit_load + set_vreg_reg.

} // namespace arm64emu
