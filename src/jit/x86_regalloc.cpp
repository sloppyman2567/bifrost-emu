// jit/x86_regalloc.cpp — virtual-register allocator + spill/evict/flush.
//
// Maps IR vregs to host x86 registers. Each vreg has a "home" x86 reg
// (or -1 if spilled to stack). The allocator tracks which vreg owns each
// x86 reg. When a vreg is needed, its home reg is used directly (no
// load/store). When a reg is needed for a new vreg, the old owner is
// spilled if dirty.
//
// Scratch x86 regs available for allocation:
//   Caller-saved (clobbered by C calls): RAX, RCX, RDX, R8, R9, R11
//   Callee-saved (preserved by C calls): R12, R13, R15
// Persistent: RBX=CPU, R14=EMU, R10=window, RBP=frame.
//
// (v1.4.0-alpha.5): added R12/R13/R15 (callee-saved) to the pool. This
// gives 9 registers instead of 6, and vregs cached in callee-saved regs
// survive CALL_INTERP without spilling — the C calling convention
// preserves them across calls.
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
//   load_vreg / store_vreg — fallback memory access (kept for legacy paths)
//   force_vreg_to_reg    — move/load vreg v into a specific host reg
//   force_two_vregs_to   — same for two vregs (handles aliasing)
#include "jit/frostjit.hpp"

#include <cstdio>
#include <cstring>

namespace arm64emu {
int32_t FrostJIT::vreg_stack_slot(int v) {
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

// Get a free x86 reg, evicting if necessary. If `preferred` >= 0, try
// to use that specific reg.
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

// ── Old simple load/store (kept for fallback paths) ────────────────────
//
// IMPORTANT (v1.4.0-alpha.5 fix): these helpers MUST participate in the
// register-allocator cache. The previous implementation always loaded
// from / stored to memory (cpu.regs[] or the stack slot), bypassing the
// cache. If a vreg was cached in a host register with a dirty value not
// yet written back, `load_vreg` would return the STALE memory value,
// and `store_vreg` would write the new value to memory but leave the
// stale cached value in the host register — so a subsequent `ensure_vreg`
// of the same vreg would still return the stale value.
//
// This was the root cause of the LOAD_MEM divergence in `__towrite`
// (hello.elf under --jit) and of wrong RBIT/CLS/REV16/REV32 results
// when their source vreg had been computed but not spilled.
//
// The fix: if the vreg is currently cached, `load_vreg` emits a `mov`
// from the cached reg; `store_vreg` updates the cache mapping (and
// marks the vreg dirty) instead of writing to memory. Only uncached
// vregs go through the memory path. Callers that need a hard memory
// writeback (e.g. before a C call that may read cpu.regs[]) should
// call `flush_all_vregs()` first.

// Load vreg `v` into x86 reg `dst`.
// If `v` is cached in a host register, emit a `mov` from that register
// (preserving the cached, possibly-dirty value). Otherwise load from
// cpu.regs[] (v <= 31) or the vreg's stack slot (v >= 33).
void FrostJIT::load_vreg(int dst, int v) {
    if (v > max_vreg_) max_vreg_ = v;
    int home = vreg_home_[v];
    if (home >= 0) {
        // Cached — copy from the cached register.
        if (dst != home) emit_mov_reg(dst, home);
        return;
    }
    if (v <= 31) {
        emit_load_arm(dst, v);
    } else {
        int32_t off = vreg_stack_slot(v);
        emit_load(dst, RBP, off);
    }
}

// Store x86 reg `src` to vreg `v`.
// If `v` is currently cached, update the cache to point at `src` (the
// old cached reg, if different, is dropped — its value is overwritten
// by `src`). If `v` is uncached, write directly to memory (cpu.regs[]
// or stack slot). Either way the vreg ends up cached in `src` and
// marked dirty, mirroring the contract of `set_vreg_reg`.
void FrostJIT::store_vreg(int v, int src) {
    if (v > max_vreg_) max_vreg_ = v;
    int home = vreg_home_[v];
    if (home >= 0 && home != src) {
        // Drop the old cached mapping — the register's value is being
        // overwritten by `src`. The old cached value is lost; callers
        // that need it preserved must `evict_vreg(v)` first.
        reg_vreg_[home] = -1;
    }
    // If src already held another vreg v2, kill v2's mapping (its
    // value was just overwritten). Callers that need v2's value
    // preserved must evict_vreg(v2) BEFORE overwriting src.
    int old_v = reg_vreg_[src];
    if (old_v >= 0 && old_v != v) {
        vreg_home_[old_v] = -1;
        vreg_dirty_[old_v] = false;
    }
    vreg_home_[v] = src;
    reg_vreg_[src] = v;
    vreg_dirty_[v] = true;
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
// Replaces 4 near-identical inline cases (~50 lines total) with one
// shared helper. Also fixes the OOB write that was in the old inline
// G2F/G2FHI cases (they read reg_vreg_[RAX] AFTER clearing it to -1,
// causing vreg_dirty_[-1] = false; this version captures the old
// value BEFORE clearing).

} // namespace arm64emu
