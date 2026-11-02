// Memory.cpp — FrostJIT memory IR-op codegen.
//
// split out of FrostJIT.cpp. This file holds the
// LOAD_MEM / STORE_MEM / LOAD_REG / STORE_REG case bodies of the IR-op
// switch, extracted into a separate method (compile_ir_mem) for
// readability. The main switch in FrostJIT.cpp dispatches to this method
// before its residual cases.
//
// No behavior change — pure file split. The method is a member of
// FrostJIT (declared in Source/FrostJIT/Compiler/FrostJIT.hpp) so it has full access
// to the JIT's emit_*, alloc_*, flush_*, etc. helpers.
//
// Return value (int — see FrostJIT.hpp):
//   -1 = op not handled here (caller falls through to next dispatcher)
//    0 = op handled, does NOT end the block
//    1 = op handled AND ends the block
// All memory ops in this file return 0 (handled, does not end block).
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include "FrostJIT/IR/IR.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace arm64emu {
// Scalar native accesses use allocator-selected scratch registers. Only the
// cold exit spills live state for a C call; successful accesses retain all
// other mappings.
bool FrostJIT::compile_guarded_mem(const IRInst& inst) {
    if (inst.op == IROp::LDXR_FAST && !inst.llsc_exclusive()) {
        // LDAR is an acquire load, not a reservation. Keep the original
        // atomic IR as an optimizer barrier; reuse only the guarded emitter.
        IRInst load = IRInst::make_load_mem(inst.dest, inst.src1,
            MemParams{inst.llsc_width(), 0}, inst.arm_pc);
        return compile_guarded_mem(load);
    }
    const bool load = inst.op == IROp::LOAD_MEM;
    if ((!load && inst.op != IROp::STORE_MEM) || !fault_sites_ ||
        !guest_memory_ || !window_base_ || !inst.arm_pc) return false;
    const MemParams mp = load ? inst.load_mem_params() : inst.store_mem_params();
    const int64_t offset = static_cast<int64_t>(mp.offset);
    if ((mp.width != 1 && mp.width != 2 && mp.width != 4 && mp.width != 8) ||
        offset < INT32_MIN || offset > INT32_MAX) return false;
    // Forced slow-store tracing disables fault_sites_, retaining the old path.
    // A pair (LDP/STP) takes this path too: translation completes both loads
    // before writing either register and writes no register before a store, so
    // a fault on either half restarts the whole instruction from unchanged state.
    DecodedInst decoded;
    if (!decode(decoded, guest_memory_->fetch_inst(fetch_address(inst.arm_pc)))) return false;

    memory_fault_stub_ = 0;
    clobber_flags(false, /*rdx_only=*/true);
    int address, value = -1;
    if (load) address = ensure_vreg(inst.src1);
    else ensure_two_vregs(inst.src1, inst.src2, &address, &value);
    const uint16_t saved_pins = pinned_host_regs_;
    pinned_host_regs_ |= (1u << address);
    if (!load) pinned_host_regs_ |= (1u << value);
    int effective = address;
    if (mp.offset) {
        effective = alloc_reg();
        pinned_host_regs_ |= (1u << effective);
        emit_byte(rex(true, effective >= 8, false, address >= 8));
        emit_byte(0x8D);
        emit_modrm_disp(effective, address, static_cast<int32_t>(mp.offset));
    }
    const int temp = alloc_reg();
    pinned_host_regs_ |= (1u << temp);
    const int page = alloc_reg();
    pinned_host_regs_ |= (1u << page);

    std::vector<size_t> cold;
    if (decoded.cls == InstClass::LDAR && mp.width > 1) {
        // Aligned x86 loads are atomic and acquire ordered. Leave unaligned
        // accesses on the existing interpreter path instead of allowing a
        // native access to tear across a cache-line/page boundary. Before the
        // gate: the checked path rejoins at the access.
        emit_mov_reg(temp, effective);
        emit_alu_imm(temp, 4, mp.width - 1);
        cold.push_back(emit_jcc_rel32_placeholder(5));
    }
    // The gate reads 0 while some page needs software checks, which can start
    // while this code runs: never hoist it out of a loop. A width without a
    // table entry compares with the window limit and tests the flag instead.
    const int outside = emit_window_bound_cmp(effective, mp.width, temp, true);
    const bool gated = outside == kJccAE;
    size_t checked = 0;
    if (gated) {
        checked = emit_jcc_rel32_placeholder(outside);
    } else {
        cold.push_back(emit_jcc_rel32_placeholder(outside));
        emit_test_software_checks();
        checked = emit_jcc_rel32_placeholder(5);  // JNE
    }
    const uint8_t required = Memory::GUEST_PAGE_MAPPED |
        (load ? Memory::GUEST_PROT_READ : Memory::GUEST_PROT_WRITE);
    const uint8_t mask = required | Memory::GUEST_PAGE_UFFD_MISSING;
    if (defer_guards_) {
        // Fast path falls through to the access; the software page checks
        // and the fault exit follow the block (flush_deferred_guards). The
        // GPR/vector state is captured here, where both cold paths divert,
        // exactly as the inline fault exit below would see it.
        DeferredGuard guard{inst, effective, temp, page, mp.width, required, mask,
                            0, checked, 0, false, gated,
                            std::move(cold), capture_guard_state()};
        guard.access = code_buf_used_;
        // Placeholder recovery offset: flush_deferred_guards patches the
        // registered site with the real stub offset before publication.
        memory_fault_stub_ = 1;
        guard.site_index = fault_site_count_.load(std::memory_order_relaxed);
        if (load) {
            emit_load_mem(page, effective, 0, mp.width, false);
            set_vreg_reg(inst.dest, page);
        } else {
            emit_store_mem(effective, 0, value, mp.width);
        }
        guard.has_site = fault_site_count_.load(std::memory_order_relaxed) == guard.site_index + 1;
        memory_fault_stub_ = 0;
        deferred_guards_.push_back(std::move(guard));
        pinned_host_regs_ = saved_pins;
        return true;
    }
    // Inline (Tier-2): the open gate or a clear flag jumps over the checks.
    const size_t bypass_pages = emit_jmp_rel32_placeholder();
    patch_jcc_rel32(checked, static_cast<int32_t>(code_buf_used_ - (checked + 6)));
    std::vector<GuardJump> passed = emit_guard_page_checks(effective, temp, page, mp.width,
                                                           required, mask, gated);
    passed.push_back({bypass_pages, true});
    const size_t cold_offset = code_buf_used_;
    for (size_t p : cold)
        patch_jcc_rel32(p, static_cast<int32_t>(cold_offset - (p + 6)));
    emit_memory_fault_exit(inst, true);
    const size_t access = code_buf_used_;
    patch_guard_jumps(passed, access);
    if (load) {
        // The result uses a scratch register, leaving the original address
        // and every mapped architectural value intact if the load faults.
        emit_load_mem(page, effective, 0, mp.width, false);
        set_vreg_reg(inst.dest, page);
    } else {
        emit_store_mem(effective, 0, value, mp.width);
    }
    pinned_host_regs_ = saved_pins;
    return true;
}
// The fault exit reads only the host-register mappings, their dirty bits and
// the vector cache's live layout and dirty flags. Tier-1's demand allocator
// may reuse an XMM between the access and deferred recovery emission.
FrostJIT::GuardState FrostJIT::capture_guard_state() const {
    GuardState state;
    for (int r = 0; r < 16; ++r) state.reg_vreg[r] = reg_vreg_[r];
    state.dirty_mask = dirty_host_regs_;
    std::memcpy(state.vec_dirty, vec_dirty_, sizeof(state.vec_dirty));
    std::memcpy(state.vec_valid, vec_valid_, sizeof(state.vec_valid));
    std::memcpy(state.vec_cache, vec_cache_, sizeof(state.vec_cache));
    std::memcpy(state.vec_owner, vec_xmm_owner_, sizeof(state.vec_owner));
    std::memcpy(state.vec_pinned, vec_pinned_, sizeof(state.vec_pinned));
    state.vec_count = vec_pinned_count_;
    state.vec_active = vec_cache_active_;
    return state;
}
void FrostJIT::swap_guard_state(GuardState& state) {
    const GuardState live = capture_guard_state();
    for (int r = 0; r < 16; ++r) {
        if (reg_vreg_[r] < 0) continue;
        vreg_home_[reg_vreg_[r]] = -1;
        vreg_dirty_[reg_vreg_[r]] = false;
        reg_vreg_[r] = -1;
    }
    for (int r = 0; r < 16; ++r) {
        const int v = state.reg_vreg[r];
        if (v < 0) continue;
        reg_vreg_[r] = v;
        vreg_home_[v] = r;
        vreg_dirty_[v] = (state.dirty_mask >> r) & 1;
    }
    dirty_host_regs_ = state.dirty_mask;
    std::memcpy(vec_dirty_, state.vec_dirty, sizeof(state.vec_dirty));
    std::memcpy(vec_valid_, state.vec_valid, sizeof(state.vec_valid));
    std::memcpy(vec_cache_, state.vec_cache, sizeof(state.vec_cache));
    std::memcpy(vec_xmm_owner_, state.vec_owner, sizeof(state.vec_owner));
    std::memcpy(vec_pinned_, state.vec_pinned, sizeof(state.vec_pinned));
    vec_pinned_count_ = state.vec_count;
    vec_cache_active_ = state.vec_active;
    state = live;
}
// Emit each deferred guard's cold code after the block: the software page
// checks (jumping back to the access when they pass) and the recoverable
// fault exit, with the state captured at the access temporarily installed.
void FrostJIT::patch_guard_jumps(const std::vector<GuardJump>& jumps, size_t target) {
    for (const GuardJump& j : jumps) {
        if (j.jmp) patch_jmp_rel32(j.offset, static_cast<int32_t>(target - (j.offset + 5)));
        else patch_jcc_rel32(j.offset, static_cast<int32_t>(target - (j.offset + 6)));
    }
}
std::vector<FrostJIT::GuardJump> FrostJIT::emit_guard_page_checks(int effective, int temp, int page,
                                                                  int width, uint8_t required,
                                                                  uint8_t mask, bool bound) {
    std::vector<size_t> fail;  // rel8 to the end of the sequence
    std::vector<GuardJump> passed;
    if (bound) {
        // A closed gate diverts every address: check the real bound first.
        fail.push_back(emit_jcc_rel8_placeholder(
            static_cast<uint8_t>(emit_window_bound_cmp(effective, width, temp, false))));
    }
    emit_mov_reg(page, effective);
    emit_shift_imm8(page, 5, 12);
    // check: temp = page_flags[page] through the pointer in the header page, so
    // the tail embeds no host address. Byte-sized and/cmp; a crossing access
    // repeats the check with its last page, which then matches and leaves.
    const size_t check = code_buf_used_;
    emit_load(temp, WIN_REG, Memory::JIT_PAGE_FLAGS_OFF);
    const uint8_t index = rex(false, temp >= 8, page >= 8, temp >= 8);
    if (index != 0x40) emit_byte(index);
    emit_byte(0x0F); emit_byte(0xB6);  // movzx temp32, byte [temp + page]
    if ((temp & 7) == 5) {
        emit_byte(modrm(1, temp & 7, 4)); emit_byte(sib(0, page & 7, temp & 7)); emit_byte(0);
    } else {
        emit_byte(modrm(0, temp & 7, 4)); emit_byte(sib(0, page & 7, temp & 7));
    }
    auto alu8 = [&](int kind, uint8_t imm) {  // and/cmp temp8, imm8 (REX for spl..dil, r8b..)
        if (temp >= 4) emit_byte(rex(false, false, false, temp >= 8));
        emit_byte(0x80); emit_byte(modrm(3, kind, temp & 7)); emit_byte(imm);
    };
    alu8(4, mask);
    alu8(7, required);
    fail.push_back(emit_jcc_rel8_placeholder(5));  // JNE
    if (width > 1) {
        emit_byte(rex(true, temp >= 8, false, effective >= 8));  // lea temp, [effective + width - 1]
        emit_byte(0x8D);
        emit_modrm_disp(temp, effective, width - 1);
        emit_shift_imm8(temp, 5, 12);
        emit_cmp_reg(temp, page);
        passed.push_back({emit_jcc_rel32_placeholder(4), false});  // JE: same page
        emit_mov_reg(page, temp);
        emit_byte(0xEB);  // jmp check
        emit_byte(static_cast<uint8_t>(static_cast<int8_t>(check - (code_buf_used_ + 1))));
    } else {
        passed.push_back({emit_jmp_rel32_placeholder(), true});
    }
    for (size_t p : fail) patch_jcc_rel8(p, static_cast<int8_t>(code_buf_used_ - (p + 2)));
    return passed;
}
constexpr bool FrostJIT::cold_stub_register(int r) {
    for (int allocatable : ALLOC_REGS)
        if (allocatable == r) return true;
    return false;
}
// Called once before a JIT's first block. A page-check stub takes the address in
// its register, preserves every register and returns ZF set when the access may
// proceed. The fault exit takes the guest PC in RAX and ends the frame, whichever
// Tier-1 block jumps to it: both frame layouts return the same way.
void FrostJIT::emit_cold_stubs() {
    const bool recording = recording_;
    recording_ = false;  // not part of the block being recorded
    const size_t start = code_buf_used_;
    static constexpr int widths[4] = {1, 2, 4, 8};
    for (int reg = 0; reg < 16; ++reg) {
        if (!cold_stub_register(reg)) continue;
        const int first = reg == RAX ? RCX : RAX;
        const int second = reg == RDX ? RCX : RDX;
        for (int w = 0; w < 4; ++w) {
            for (int write = 0; write < 2; ++write) {
                guard_stub_[reg][w][write] = static_cast<uint32_t>(code_buf_used_);
                const uint8_t required = Memory::GUEST_PAGE_MAPPED |
                    (write ? Memory::GUEST_PROT_WRITE : Memory::GUEST_PROT_READ);
                emit_push(first);
                emit_push(second);
                const std::vector<GuardJump> passed = emit_guard_page_checks(
                    reg, first, second, widths[w], required,
                    required | Memory::GUEST_PAGE_UFFD_MISSING, true);
                // test rsp,rsp: a nonzero register clears ZF for the failing path.
                emit_byte(0x48); emit_byte(0x85); emit_byte(0xE4);
                patch_guard_jumps(passed, code_buf_used_);
                emit_pop(second);
                emit_pop(first);
                emit_ret();
            }
        }
    }
    fault_exit_stub_ = static_cast<uint32_t>(code_buf_used_);
    emit_store(CPU_REG, PC_OFF, RAX);
    emit_load_emu(RDI);
    emit_mov_reg(RSI, CPU_REG);
    emit_push(RAX);  // aligns the call; the frame return drops it
    emit_call_abs(reinterpret_cast<void*>(&jit_fault_step));
    emit_frame_return(false);
    recording_ = recording;
    if (code_buf_overflow_) {
        code_buf_used_ = start;
        code_buf_overflow_ = false;
        return;
    }
    cold_stubs_ready_ = true;
}
void FrostJIT::flush_deferred_guards() {
    for (DeferredGuard& g : deferred_guards_) {
        patch_jcc_rel32(g.sw_jump, static_cast<int32_t>(code_buf_used_ - (g.sw_jump + 6)));
        if (g.gated && use_cold_stubs() && cold_stub_register(g.effective)) {
            const int width = g.width == 1 ? 0 : g.width == 2 ? 1 : g.width == 4 ? 2 : 3;
            const uint32_t target = guard_stub_[g.effective][width][(g.required & Memory::GUEST_PROT_WRITE) ? 1 : 0];
            emit_byte(0xE8);  // call rel32
            emit_u32(static_cast<uint32_t>(target - (code_buf_used_ + 4)));
            const size_t passed = emit_jcc_rel32_placeholder(4);  // JE
            patch_jcc_rel32(passed, static_cast<int32_t>(g.access - (passed + 6)));
        } else {
            patch_guard_jumps(emit_guard_page_checks(g.effective, g.temp, g.page, g.width,
                                                     g.required, g.mask, g.gated), g.access);
        }
        const size_t stub = code_buf_used_;
        for (size_t p : g.cold)
            patch_jcc_rel32(p, static_cast<int32_t>(stub - (p + 6)));
        swap_guard_state(g.state);
        emit_memory_fault_exit(g.inst, true);
        swap_guard_state(g.state);
        if (g.has_site) {
            const uint64_t entry = fault_sites_[g.site_index].load(std::memory_order_relaxed);
            fault_sites_[g.site_index].store((entry & ~uint64_t(0xFFFFFFFF)) | stub,
                                             std::memory_order_relaxed);
        }
    }
    deferred_guards_.clear();
    memory_fault_stub_ = 0;
}
// ── FrostJIT::compile_ir_mem ───────────────────────────────────────────
// Handles LOAD_MEM, STORE_MEM, LOAD_REG, STORE_REG. These are the only
// IR ops that touch cpu.regs[]/cpu.v_lo[]/memory directly via the
// regalloc-aware helpers (load_vreg_to_reg, store_reg_to_vreg,
// emit_load_mem, emit_store_mem, emit_load_arm, emit_store_arm).
int FrostJIT::compile_ir_mem(const IRInst& inst) {
    switch (inst.op) {
        case IROp::LOAD_REG:
            // dest = arm64_reg[src1]. src1 is the ARM64 reg index.
            //
            // If the ARM reg vreg (src1, in 0..31) is already cached in a
            // host reg — e.g., because a previous FP_F2I / FP_I2F / FMOV_F2G
            // wrote directly to it via store_reg_to_vreg(src1, RAX) without
            // going through STORE_REG — reuse the cached value. Otherwise
            // we would emit a load from cpu.regs[src1], which is STALE
            // (the dirty vreg hasn't been flushed yet). This was the root
            // cause of `fcvtzs w1, d0; add w19, w19, w1` losing the
            // conversion result: FP_F2I cached vreg 1, then LOAD_REG v34, x1
            // reloaded the stale cpu.regs[1] instead of the cached vreg 1.
            //
            // instead of cpu.regs[]. This is used for FP LDR/STR.
            {
                kill_vreg(inst.dest);
                int d;
                if (inst.src1 <= 31 && vreg_home_[inst.src1] >= 0) {
                    int s = vreg_home_[inst.src1];
                    d = alloc_reg_excluding(s, -1);
                    emit_mov_reg(d, s);
                } else if (inst.is_fp_load()) {
                    // FP register: load from cpu.v_lo[src1] (or, when the fp
                    // cache pins src1, the pinned XMM — reg-reg move).
                    d = alloc_reg();
                    int xs = vec_xmm(inst.src1);
                    if (xs >= 0) {
                        emit_vmovq_xmm_to_gpr(d, xs);
                    } else {
                        emit_load(d, CPU_REG, V_LO_OFF + 8 * inst.src1);
                    }
                } else {
                    d = alloc_reg();
                    emit_load_arm(d, inst.src1);
                }
                set_vreg_reg(inst.dest, d);
            }
            return 0;
        case IROp::STORE_REG:
            // arm64_reg[dest] = src1. Write to cpu.regs[dest] (or v_lo if is_fp).
            //
            // Phase 2 (self-loop / tier-2 region): KEEP dest cached after the
            // store and DEFER the cpu.regs[] write. The value stays in a
            // register (a pinned loop-carried vreg in R12-R15, or the
            // transferred reg of a dead src1) marked DIRTY; the block's own
            // later LOAD_REG reads reuse the cached register, and the loop
            // exit (self-loop fall-through / region exit, both of which run
            // flush_all_vregs before returning OR chaining) writes it back to
            // cpu.regs[dest] ONCE. Per loop iteration this replaces a store→
            // cpu.regs→load round trip with pure register traffic.
            //
            // Deferral is safe ONLY because keep_store_dest_ is enabled
            // exclusively for self-loop blocks and back-edge regions whose
            // exits always flush: a self-loop's taken edge is the loop-back
            // (taken target == start_pc, so the exit is the fall-through →
            // shared epilogue with flush_all_vregs), and regions flush at
            // every cold/side exit. Call-like ops (interpreter, BL/BLR, SVC)
            // are excluded from pinning — they read cpu.regs[] directly and
            // would see the stale (deferred) value.
            //
            //   * Pinned arch vreg (arch_pin_[dest] >= 0): move src1 into the
            //     pin (the loop reads it from there), mark dest dirty.
            //   * Unpinned, src1 dead after this op: transfer src1's register
            //     to dest (dest stays hot for its later reads), dirty.
            //   * src1 == dest: dest's own reg already holds the value, dirty.
            //   * Otherwise (src1 still live): old behavior — eager store to
            //     cpu.regs[dest], kill the dest mapping.
            // src1 itself is never touched (stays cached in its reg).
            //
            // instead of cpu.regs[]. This is used for FP LDR/STR.
            {
                // A kept dest must live in a register the BRCOND term leaves
                // intact: the term's flag-prep + mov-imm clobber RAX/RCX/RDX/
                // R8 (FLAGS3) EVERY iteration, and a kept dest in one of
                // those would be read back garbage by the next iteration's
                // LOAD_REG (compiled as a direct reg→reg mov). Pins (R12-R15)
                // and R9/R11 are untouched by the term, so only those are
                // safe. A register that is another vreg's pin is also unsafe
                // to steal (it holds that vreg's loop-carried value).
                if (!inst.is_fp_store() && inst.dest <= 30 && keep_store_dest_) {
                    int pin = arch_pin_[inst.dest];
                    if (pin >= 0) {
                        int s = ensure_vreg(inst.src1);
                        if (s != pin) emit_mov_reg(pin, s);
                        if (vreg_home_[inst.dest] != pin) {
                            if (vreg_home_[inst.dest] >= 0) {
                                dirty_host_regs_ &= ~(1u << vreg_home_[inst.dest]);
                                reg_vreg_[vreg_home_[inst.dest]] = -1;
                            }
                            vreg_home_[inst.dest] = pin;
                            reg_vreg_[pin] = inst.dest;
                        }
                        // Deferred store: dest lives in its pin, dirty.
                        vreg_dirty_[inst.dest] = true;
                        dirty_host_regs_ |= (1u << pin);
                        vreg_last_use_[inst.dest] = ++regalloc_lru_counter_;
                        return 0;
                    }
                    int s = ensure_vreg(inst.src1);
                    const bool s_safe =
                        (s != RAX && s != RCX && s != RDX && s != R8) &&
                        !(pinned_host_regs_ & (1u << s));
                    if (s_safe && inst.src1 == inst.dest) {
                        // Self-store: dest's own reg already holds the value.
                        vreg_dirty_[inst.dest] = true;
                        dirty_host_regs_ |= (1u << s);
                        vreg_last_use_[inst.dest] = ++regalloc_lru_counter_;
                        return 0;
                    }
                    if (s_safe &&
                        vreg_last_use_op_[inst.src1] <= static_cast<int>(cur_op_index_)) {
                        // src1 dead after this op — transfer its reg to dest.
                        if (vreg_home_[inst.dest] >= 0) {
                            dirty_host_regs_ &= ~(1u << vreg_home_[inst.dest]);
                            reg_vreg_[vreg_home_[inst.dest]] = -1;
                            vreg_home_[inst.dest] = -1;
                        }
                        vreg_home_[inst.dest] = s;
                        reg_vreg_[s] = inst.dest;
                        vreg_home_[inst.src1] = -1;
                        vreg_dirty_[inst.src1] = false;
                        vreg_dirty_[inst.dest] = true;
                        dirty_host_regs_ |= (1u << s);
                        vreg_last_use_[inst.dest] = ++regalloc_lru_counter_;
                        return 0;
                    }
                    // src1 still live (or its reg is unsafe) — eager store.
                    emit_store_arm(inst.dest, s);
                    kill_vreg(inst.dest);
                    return 0;
                }
                int s = ensure_vreg(inst.src1);
                if (inst.is_fp_store() && inst.dest <= 30) {
                    // FP register: store to cpu.v_lo[dest] (or, when the fp
                    // cache pins dest, the pinned XMM — reg-reg move + dirty).
                    int xd = vec_xmm(inst.dest);
                    if (xd >= 0) {
                        emit_vmovq_gpr_to_xmm(xd, s);
                        vec_cache_mark_dirty(inst.dest);
                    } else {
                        emit_store(CPU_REG, V_LO_OFF + 8 * inst.dest, s);
                    }
                } else {
                    emit_store_arm(inst.dest, s);
                }
                // Kill any stale dest mapping (dest's value is now in cpu.regs/v_lo).
                if (inst.dest <= 31) {
                    kill_vreg(inst.dest);
                }
            }
            return 0;
        case IROp::LOAD_MEM: {
            const MemParams mp = inst.load_mem_params();
            // Memory access via emit_load_mem. The slow path calls
            // jit_load_mem_slow (clobbers caller-saved regs); the fast
            // path uses RAX/RDX/RCX/R10 internally. Now that
            // load_vreg_to_reg is cache-aware, we only need to flush
            // caller-saved dirty vregs — callee-saved vregs (R12/R13/
            // R15) survive the C call and load_vreg_to_reg will mov
            // from them correctly.
            //
            // replaced O(max_vreg_) flush + 6-reg
            // invalidate loop with two O(popcount) bitmask walks.
            clobber_flags();
            constexpr uint16_t MEM_CLOBBER =
                (1u << RAX) | (1u << RCX) | (1u << RDX) |
                (1u << R8)  | (1u << R9)  | (1u << R11);
            // Fast path: if src1 is a dead scratch vreg already cached in
            // RAX, keep it there (skip the flush→reload sandwich). The
            // address is consumed by emit_load_mem, which then OVERWRITES
            // RAX with the loaded data — so drop src1's mapping afterwards
            // (safe: src1 is dead, and its value was consumed as the base).
            //
            // Direct-index fast path (indexed addressing, 2026-08-21): the
            // emitter NEVER destroys the address register, so a src1 cached
            // in a CALLEE-SAVED reg (an arch pin — the common loop-carried
            // pointer shape) rides the SIB index directly: no staging copy
            // into RAX at all. Only taken when neither keep applies; the
            // full MEM_CLOBBER flush+invalidate still protects other live
            // vregs from the slow-path C call.
            const bool keep_rax =
                vreg_fast_keep_candidate(inst.src1, RAX, inst.dest);
            const int ahome = vreg_home_[inst.src1];
            if (!keep_rax && ahome >= R12 && ahome <= R15) {
                flush_dirty_host_regs(MEM_CLOBBER);
                flush_scratch_host_regs(MEM_CLOBBER);
                invalidate_host_regs(MEM_CLOBBER);
                emit_load_mem(RAX, ahome, static_cast<int32_t>(mp.offset),
                              mp.width, false);
            } else {
                uint16_t kept = load_vreg_to_reg_fast(RAX, inst.src1,
                                                      inst.dest, MEM_CLOBBER);
                emit_load_mem(RAX, RAX, static_cast<int32_t>(mp.offset),
                              mp.width, false);
                if (kept & (1u << RAX)) kill_vreg(inst.src1);
            }
            // Keep dest cached in RAX (set_vreg_reg) instead of storing to
            // memory: the loaded value is usually consumed immediately
            // (STORE_REG / next op), and store_reg_to_vreg would kill the
            // mapping so the consumer reloads from the stack slot — a
            // redundant sandwich. The epilogue flushes dest if the block ends
            // before it's read.
            set_vreg_reg(inst.dest, RAX);
            return 0;
        }
        case IROp::STORE_MEM: {
            const MemParams mp = inst.store_mem_params();
            // Same as LOAD_MEM: only flush caller-saved dirty vregs.
            clobber_flags();
            constexpr uint16_t MEM_CLOBBER =
                (1u << RAX) | (1u << RCX) | (1u << RDX) |
                (1u << R8)  | (1u << R9)  | (1u << R11);
            // Fast path for both operands: if src1/src2 are dead scratch
            // vregs already cached in RAX/RCX, keep them there. emit_store_mem
            // (indexed-address variant, 2026-08-21) PRESERVES both RAX (the
            // addr rides the SIB index — never copied or destroyed) and RCX
            // (pushed/popped around the slow call), so kept mappings stay
            // valid after the store. Decide BOTH operands BEFORE flushing
            // so src2's cache isn't wiped by src1's flush, then
            // flush+invalidate only the non-kept regs.
            bool keep1 = vreg_fast_keep_candidate(inst.src1, RAX, inst.dest);
            bool keep2 = vreg_fast_keep_candidate(inst.src2, RCX, inst.dest);
            // Direct-index fast path (2026-08-21): operands cached in
            // CALLEE-SAVED regs (arch pins) are passed straight to the
            // emitter — the address rides the SIB index and the value is
            // read from its own register, so the pin→RAX/RCX staging movs
            // disappear. Only when neither keep applies (a keep is cheaper
            // still); the full MEM_CLOBBER flush+invalidate still runs.
            const int ahome = vreg_home_[inst.src1];
            const int vhome = vreg_home_[inst.src2];
            if (!keep1 && !keep2 &&
                ahome >= R12 && ahome <= R15 &&
                vhome >= R12 && vhome <= R15) {
                flush_dirty_host_regs(MEM_CLOBBER);
                flush_scratch_host_regs(MEM_CLOBBER);
                invalidate_host_regs(MEM_CLOBBER);
                emit_store_mem(ahome, static_cast<int32_t>(mp.offset),
                               vhome, mp.width);
                return 0;
            }
            uint16_t kept_mask = 0;
            if (keep1) kept_mask |= (1u << RAX);
            if (keep2) kept_mask |= (1u << RCX);
            flush_dirty_host_regs(MEM_CLOBBER & ~kept_mask);
            flush_scratch_host_regs(MEM_CLOBBER & ~kept_mask);
            invalidate_host_regs(MEM_CLOBBER & ~kept_mask);
            if (!keep1) load_vreg_to_reg(RAX, inst.src1);
            if (!keep2) load_vreg_to_reg(RCX, inst.src2);
            emit_store_mem(RAX, static_cast<int32_t>(mp.offset), RCX, mp.width);
            return 0;
        }
        default:
            return -1;  // not handled — caller falls through
    }
}
} // namespace arm64emu
