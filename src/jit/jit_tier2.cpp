// Tier-2 bounded loop discovery, region optimization and publication.
// Conditional branches follow measured hot successors; direct jumps and capped
// straight-line blocks continue inline. Collection emits no host code.
// Unsupported instructions abort rather than crossing a fault/call boundary.
//
// Regalloc contract (IMPORTANT): translate_to_ir allocates scratch vregs from
// the thread_local g_alloc (vregs 0-32 = arch regs, scratch >= 33).
// translate_block resets it per block via ir_reset_vreg_alloc(); this walker
// does the same, so collection never consumes the global allocator. Each
// block's IRBlock.insts is self-contained (scratch numbering reused across
// blocks is fine — the region compiler remaps/re-allocates later). A guard
// before each instruction aborts the trace if the current block's allocation
// approaches the 4095-entry vreg table.
#include "jit/frostjit.hpp"
#include "core/emulator.h"
#include "debug_flags.h"
#include "ir/ir.hpp"
#include "ir/ir.h"
#include <chrono>
#include <time.h>
#include <algorithm>  // std::find
#include <cstdint>
#include <cstdio>
#include <cstring>  // memcpy
#include <vector>
namespace arm64emu {
namespace {
// Codegen-state snapshot captured at each terminating-branch JCC so the
// deferred side-exit / loop-back-edge epilogues can be emitted LATER (after
// the whole region body) with the exact regalloc/flags state of that branch
// point. The region body is one flat code stream with NO branch-patch jumps
// between its blocks (internal fall-through edges keep the codegen state
// live), so the only state a deferred epilogue needs is the state at its own
// branch — restored here, then materialize/flush/store emit as-of that point.
struct RegionSnapshot {
    bool   flags_in_host_ = false;
    bool   flags_from_sub_ = false;
    bool   need_cmc_ = false;   // the branch needed a `cmc` before its JCC
    uint16_t dirty_host_regs_ = 0;
    int    reg_vreg_[16];
    std::vector<int>     vreg_home_;
    std::vector<uint8_t> vreg_dirty_;
};
}  // namespace

// Emitted after all guest state is flushed at a Tier-1 conditional exit.
// R11 is free here, RAX retains next_pc, and NZCV is already materialized.
void FrostJIT::emit_tier2_edge_profile(bool taken) {
    if (!tier2_branch_profile_) return;
    emit_mov_imm64(R11, reinterpret_cast<uint64_t>(tier2_branch_profile_));
    emit_byte(0x41); emit_byte(0x83); emit_byte(0x3B); emit_byte(0); // cmp [r11],0
    const size_t disabled = emit_jcc_rel32_placeholder(4);
    const uint8_t offset = taken ? offsetof(Tier2BranchProfile, taken)
                                 : offsetof(Tier2BranchProfile, fallthrough);
    // Each outcome stops accumulating at 4096 (MT can overshoot by the
    // number of concurrent CPUs). No counter wraps or unbounded profiling.
    emit_byte(0x41); emit_byte(0x81); emit_byte(0x7B); emit_byte(offset);
    emit_u32(4096);
    const size_t saturated = emit_jcc_rel32_placeholder(3); // JAE
    if (mt_active()) emit_byte(0xF0);
    emit_byte(0x41); emit_byte(0xFF); emit_byte(0x43); emit_byte(offset); // inc [r11+off]
    for (size_t p : {disabled, saturated})
        patch_jcc_rel32(p, static_cast<int32_t>(code_buf_used_ - (p + 6)));
}
void FrostJIT::invalidate_tier2_profiles(uint64_t lo, uint64_t hi) {
    for (auto it = tier2_profiles_by_pc_.begin(); it != tier2_profiles_by_pc_.end();) {
        if (it->first >= lo && it->first < hi) {
            it->second->enabled.store(0, std::memory_order_release);
            it = tier2_profiles_by_pc_.erase(it);
        } else ++it;
    }
}
void FrostJIT::clear_tier2_profiles() {
    for (auto& profile : tier2_branch_profiles_)
        profile->enabled.store(0, std::memory_order_release);
    tier2_profiles_by_pc_.clear();
}

static uint64_t tier2_probe_hash(uint64_t pc) {
    pc ^= pc >> 33; pc *= 0xff51afd7ed558ccdull;
    pc ^= pc >> 33; pc *= 0xc4ceb9fe1a85ec53ull;
    pc ^= pc >> 33;
    return pc;
}

// Only private sampling copies carry probes. Public Tier-1 and selected
// region code has no dormant guard, counter, or scratch-register save.
void FrostJIT::emit_tier2_work_probe(uint64_t pc, unsigned count) {
    if (!tier2_sampling_compile_) return;
    emit_push(RAX); emit_byte(0x9C);
    emit_byte(0x81); emit_byte(0x83);
    emit_u32(offsetof(CPU, jit_tier2_trial_work)); emit_u32(count);
    emit_mov_imm_to_rax(tier2_probe_hash(pc));
    emit_byte(0x48); emit_byte(0x01); emit_byte(0x83);
    emit_u32(offsetof(CPU, jit_tier2_trial_hash));
    emit_byte(0x9D); emit_pop(RAX);
}

static uint64_t measured_region_entry(CPU* cpu, Emulator* emu, void* trial) {
    return emu->jit()->tier2_measure(*cpu, *emu, trial);
}
static uint64_t thread_cpu_ns() {
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

uint64_t FrostJIT::tier2_measure(CPU& cpu, Emulator& emu, void* opaque) {
    auto& trial = *static_cast<Tier2Trial*>(opaque);
    auto* monitor = trial.monitor;
    if (!monitor->enabled.load(std::memory_order_acquire)) return cpu.pc;
    unsigned decision = trial.decision.load(std::memory_order_acquire);
    if (decision) return (decision == 1 ? trial.candidate : trial.baseline.fn)(&cpu, &emu);
    auto& slot = cpu.jit_tier2_feedback[monitor->slot];
    if (slot.owner != monitor->identity) {
        slot = CPU::Tier2FeedbackSlot{};
        slot.owner = monitor->identity;
    }
    const bool candidate = slot.baseline_ns != 0;
    auto fn = candidate ? (trial.candidate_sample ? trial.candidate_sample : trial.candidate)
                        : (trial.baseline_sample ? trial.baseline_sample : trial.baseline.fn);
    // Never replay guest memory or restore architectural state: each arm runs
    // once on the live workload. Matched work/exit signatures make samples
    // comparable; CLOCK_THREAD_CPUTIME_ID excludes scheduler descheduling.
    struct Reset {
        CPU& cpu;
        ~Reset() {
            cpu.jit_tier2_trial = nullptr;
            cpu.jit_tier2_trial_fn = cpu.jit_tier2_trial_chain_fn = nullptr;
        }
    } reset{cpu};
    cpu.jit_tier2_trial = &trial;
    cpu.jit_tier2_trial_fn = fn;
    cpu.jit_tier2_trial_chain_fn = candidate
        ? (trial.candidate_chain_sample ? trial.candidate_chain_sample : trial.candidate_chain)
        : (trial.baseline_chain_sample ? trial.baseline_chain_sample : trial.baseline.chain_entry);
    cpu.jit_tier2_trial_remaining = slot.sample_quota;
    cpu.jit_tier2_trial_work = cpu.jit_tier2_trial_hash = 0;
    const uint64_t start = thread_cpu_ns();
    for (unsigned dispatches = 0; dispatches < 2048; ++dispatches) {
        cpu.jit_chain_budget = 256;
        fn(&cpu, &emu);
        if (!cpu.jit_tier2_trial_remaining || !cpu.running ||
            !monitor->enabled.load(std::memory_order_acquire) || trial.decision.load(std::memory_order_acquire) || cpu.pc < trial.lo || cpu.pc >= trial.hi || cpu.pc == cpu.jit_stop_pc) break;
        // A lease can expire at a Tier-1 constituent. Resume without compile
        // work in the timed interval, using retained immutable function refs.
        fn = nullptr;
        if (cpu.pc == monitor->head) {
            // The native edge lease can expire before reaching the head's
            // trial gate. Account for that completed iteration here as well.
            if (!--cpu.jit_tier2_trial_remaining) break;
            fn = cpu.jit_tier2_trial_fn;
        }
        else for (const auto& block : trial.baseline_blocks)
            if (block.first == cpu.pc) { fn = block.second; break; }
        if (!fn) break;
    }
    const uint64_t end = thread_cpu_ns();
    const uint64_t ns = end > start && start ? end - start : 0;
    const uint32_t work = cpu.jit_tier2_trial_work;
    const uint64_t hash = cpu.jit_tier2_trial_hash;
    const uint64_t exit = cpu.pc;
    cpu.jit_tier2_trial = nullptr;
    bool finish = false, keep = false;
    {
        std::lock_guard<std::mutex> sample_lock(trial.mutex);
        if (trial.decision.load(std::memory_order_relaxed)) return exit;
        ++trial.attempts;
        // A pair belongs to one vCPU. Reject short, different-path and
        // different-work samples instead of comparing unrelated executions.
        if (monitor->enabled.load(std::memory_order_acquire) && ns && (candidate || ns >= 10000) && !cpu.jit_tier2_trial_remaining && exit == monitor->head && work >= 128) {
            if (!candidate) {
                slot.baseline_ns = ns; slot.baseline_exit = exit;
                slot.baseline_work = work; slot.baseline_hash = hash;
            } else if (slot.baseline_exit == exit && slot.baseline_work == work && slot.baseline_hash == hash) {
                trial.ratios.push_back(double(ns) / double(slot.baseline_ns));
                tier2_measured_pairs.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (candidate) slot.baseline_ns = 0;
        else if (ns && ns < 10000 && !cpu.jit_tier2_trial_remaining && slot.sample_quota < 4096)
            slot.sample_quota *= 2; // amortize timer cost on very fast loops
        if (trial.ratios.size() >= 8) {
            auto ratios = trial.ratios;
            std::sort(ratios.begin(), ratios.end());
            const double median = (ratios[3] + ratios[4]) / 2;
            const unsigned wins = std::count_if(ratios.begin(), ratios.end(), [](double r) { return r < 0.97; });
            keep = median < 0.97 && wins >= 6;
            finish = true;
            if (tier2_trace_enabled())
                fprintf(stderr, "[tier2] measured head=%llx pairs=%zu candidate/baseline=%.4f wins=%u keep=%d\n",
                    (unsigned long long)monitor->head, ratios.size(), median, wins, keep);
        } else if (trial.attempts >= 128) finish = true; // insufficient proof keeps Tier-1
        if (finish) trial.decision.store(keep ? 1 : 2, std::memory_order_release);
    }
    if (finish) finish_tier2_trial(trial, keep);
    return exit;
}

void FrostJIT::redirect_tier2_head_locked(uint64_t pc) {
    auto target = blocks_.find(pc);
    if (target == blocks_.end()) return;
    refresh_mt_edges(pc);
    invalidate_dispatch_cache();
    if (mt_active()) return;
    const auto* fn = reinterpret_cast<const uint8_t*>(chain_skip_enabled() && target->second.chain_entry
        ? target->second.chain_entry : target->second.fn);
    auto refs = back_refs_.find(pc);
    if (refs == back_refs_.end()) return;
    make_writable();
    for (uint64_t source : refs->second) {
        auto src = blocks_.find(source);
        if (src == blocks_.end() || src->second.is_region) continue;
        for (auto edge : {std::pair<uint64_t, size_t>{src->second.chain_target_pc, src->second.chain_patch_off},
                           {src->second.taken_chain_target_pc, src->second.taken_chain_patch_off}}) {
            if (edge.first != pc || !edge.second || edge.second + 5 > CODE_BUF_SIZE) continue;
            code_buf_[edge.second] = 0xE9;
            const int32_t rel = static_cast<int32_t>(fn - (code_buf_ + edge.second + 5));
            memcpy(code_buf_ + edge.second + 1, &rel, 4);
            if (edge.second == src->second.chain_patch_off) src->second.chained = true;
            if (edge.second == src->second.taken_chain_patch_off) src->second.taken_chained = true;
        }
    }
    std::atomic_thread_fence(std::memory_order_release);
    make_executable();
}

// Private ST chains are never registered in the public back-reference graph.
// Revoke them before the second CPU starts; MT copies use data cells only.
void FrostJIT::revoke_tier2_samples_locked(Tier2Trial& trial) {
    if (trial.sampling_revoked) return;
    trial.sampling_revoked = true;
    for (auto* edge : trial.sampling_edges) {
        edge->live = false;
        edge->target.store(0, std::memory_order_release);
    }
    for (auto& item : trial.sampling_blocks) {
        auto& block = item.second;
        for (size_t off : {block.chain_patch_off, block.taken_chain_patch_off}) {
            if (!off || off + 5 > CODE_BUF_SIZE || mt_edge_slots_.count(off)) continue;
            if (code_buf_[off] != 0xE9) continue;
            make_writable();
            std::memset(code_buf_ + off, 0x90, 5);
            if (!chain_skip_enabled()) code_buf_[off] = 0xC3;
            make_executable();
        }
    }
}

void FrostJIT::finish_tier2_trial(Tier2Trial& trial, bool keep) {
    std::unique_lock<std::shared_mutex> lock(blocks_mutex_);
    auto head = blocks_.find(trial.monitor->head);
    if (head == blocks_.end() || head->second.tier2_monitor != trial.monitor ||
        !trial.monitor->enabled.load(std::memory_order_acquire)) return;
    revoke_tier2_samples_locked(trial);
    if (keep) {
        head->second = trial.candidate_entry;
        for (auto* edge : trial.baseline_edges) { edge->live = false; edge->target.store(0, std::memory_order_release); }
        tier2_measured_keeps.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Preserve the monitor on the restored baseline so in-flight trial
        // wrappers are revoked by subsequent mapping invalidation too.
        const auto dependencies = head->second.inlined_ranges_;
        head->second = trial.baseline;
        head->second.inlined_ranges_ = dependencies;
        head->second.tier2_monitor = trial.monitor;
        for (auto* edge : trial.candidate_edges) { edge->live = false; edge->target.store(0, std::memory_order_release); }
        ++tier2_retrains_[trial.monitor->head];
        tier2_measured_rejections.fetch_add(1, std::memory_order_relaxed);
    }
    redirect_tier2_head_locked(trial.monitor->head);
}

// Tail-called only after the generated frame has been dismantled. All guest
// state is current, so cache revocation cannot strand live register mappings.
static uint64_t tier2_feedback_exit(CPU* cpu, Emulator* emu, void* monitor) noexcept {
    try { if (emu && emu->jit()) emu->jit()->tier2_region_feedback(*cpu, monitor); }
    catch (...) {} // never unwind into the generated caller
    return cpu->pc;
}
void FrostJIT::tier2_region_feedback(CPU& cpu, void* opaque) {
    auto* monitor = static_cast<Tier2RegionMonitor*>(opaque);
    if (cpu.jit_tier2_trial || (monitor && monitor->trial &&
        monitor->trial->decision.load(std::memory_order_acquire) != 1)) return;
    if (!monitor || !monitor->enabled.load(std::memory_order_acquire)) return;
    auto& sample = cpu.jit_tier2_feedback[monitor->slot];
    if (sample.owner != monitor->identity) {
        sample.owner = monitor->identity;
        sample.pressure = 0;
    }
    if (sample.credit) {
        sample.credit = 0;
        if (sample.pressure) --sample.pressure;
        return;
    }
    if (++sample.pressure < 64) return;
    sample.pressure = 0;
    std::unique_lock<std::shared_mutex> lock(blocks_mutex_);
    auto head = blocks_.find(monitor->head);
    if (head == blocks_.end() || head->second.tier2_monitor != monitor ||
        !monitor->enabled.load(std::memory_order_relaxed)) return;
    // Select the region and its Tier-1 constituents precisely. Instruction
    // bytes did not change, so neighboring regions need not be discarded.
    std::vector<uint64_t> victims;
    const auto& ranges = head->second.inlined_ranges_;
    for (const auto& kv : blocks_) {
        if (kv.first == monitor->head) { victims.push_back(kv.first); continue; }
        if (kv.second.is_region) continue;
        const uint64_t end = kv.first + kv.second.instr_count * 4ull;
        for (const auto& range : ranges)
            if (kv.first < range.first + range.second * 4ull && end > range.first) {
                victims.push_back(kv.first); break;
            }
    }
    for (const auto& range : ranges)
        invalidate_tier2_profiles(range.first, range.first + range.second * 4ull);
    monitor->enabled.store(0, std::memory_order_release);
    ++tier2_retrains_[monitor->head];
    tier2_phase_retirements.fetch_add(1, std::memory_order_relaxed);
    // Version check, selection and revocation share one critical section:
    // a concurrent replacement cannot be accidentally retired afterward.
    retire_blocks_locked(victims);
}

Tier2Trace FrostJIT::collect_tier2_trace(Emulator& emu, uint64_t head_pc) {
    Tier2Trace trace;
    trace.head_pc = head_pc;

    // Trace caps (ROADMAP #14: "start 64 blocks or ~2048 guest instructions").
    constexpr int       MAX_TRACE_BLOCKS = 64;
    constexpr uint64_t  MAX_TRACE_INSTS  = 2048;
    // Vreg ceiling: guard before allocating (the per-block scratch space is
    // bounded by the 4095-entry uint16_t vreg table).
    constexpr uint16_t  VREG_LIMIT = 4000;
    // Same block-size cap as translate_block.
    constexpr int       MAX_BLOCK_REG_PRESSURE = 32;

    // Follow fall-through and direct-jump edges. Only a return to the head
    // closes a region loop; other cycles are bounded by the visited set.
    std::vector<uint64_t> visited;

    uint64_t cur_pc = head_pc;
    bool trace_ended = false;  // normal termination (RET / B / caps / cold-entry)
    bool aborted = false;      // abort (trace unusable, ok stays false)

    while (!trace_ended && !aborted && trace.blocks.size() < MAX_TRACE_BLOCKS) {
        if (trace.total_insts >= MAX_TRACE_INSTS) {
            trace.stop_reason = "inst_cap";
            trace_ended = true;
            break;
        }
        // NOTE: there is deliberately NO "cold_entry" stop here anymore.
        // Real hot loops have ALL their blocks already translated (they ran
        // long enough to be hot), so stopping at the first cached block used
        // to abort every real trace at 1 block. The region compiler re-compiles
        // trace blocks from their IR regardless of whether a standalone block
        // exists, so walking through already-translated guest code is correct.
        // Only the head may be revisited (as a back-edge).
        if (cur_pc != head_pc &&
            std::find(visited.begin(), visited.end(), cur_pc) != visited.end()) {
            trace.stop_reason = "revisit";
            trace_ended = true;
            break;
        }

        // Do not build overlapping regions through an already optimized
        // head. Entering its published region retains that optimization and
        // avoids spending code-cache space on rotated copies of the loop.
        const auto cached = blocks_.find(cur_pc);
        if (cur_pc != head_pc && cached != blocks_.end() && cached->second.is_region) {
            trace.stop_reason = "region_entry";
            aborted = true;
            break;
        }

        Tier2Trace::Block tb;
        tb.pc = cur_pc;
        tb.ir.start_pc = cur_pc;
        visited.push_back(cur_pc);
        // Fresh scratch numbering per block (see the file header — collection
        // must NOT consume the global allocator).
        ir_reset_vreg_alloc();

        uint64_t ip = cur_pc;
        int instr_count = 0;
        bool block_ended = false;
        bool trace_ends_after_block = false;
        uint64_t next_pc = cur_pc;

        while (!block_ended && instr_count < MAX_BLOCK_REG_PRESSURE) {
            if (trace.total_insts + static_cast<uint64_t>(instr_count) >= MAX_TRACE_INSTS) {
                // Mid-block instruction cap — a normal (bounded) end.
                trace.stop_reason = "inst_cap";
                trace_ended = true;
                block_ended = true;
                break;
            }
            if (g_alloc.next > VREG_LIMIT) {
                trace.stop_reason = "vreg_exhaust";
                aborted = true;
                break;
            }

            uint32_t inst;
            try {
                inst = emu.mem().fetch_inst(ip);
            } catch (...) {
                trace.stop_reason = "decode_fail";
                aborted = true;
                break;
            }
            DecodedInst d;
            if (!decode(d, inst)) {
                trace.stop_reason = "decode_fail";
                aborted = true;
                break;
            }

            // ── Trace-aborting instruction classes (never in a region) ──
            static const bool no_callregion_ =
                (getenv("BIFROST_NO_CALLREGION") != nullptr);
            if (d.cls == InstClass::SVC || d.cls == InstClass::SVC_IMM) {
                trace.stop_reason = "svc";
                aborted = true;
                break;
            }
            if (d.cls == InstClass::BR) {
                // An indirect `br xN` is a data-dependent JUMP (RET is its own
                // InstClass) — the walker can't follow it.
                trace.stop_reason = "indirect_br";
                aborted = true;
                break;
            }
            if (d.cls == InstClass::BL) {
                if (!no_callregion_ &&
                    lookup_only_locked(ip + static_cast<uint64_t>(d.imm)) != nullptr) {
                    // Call-aware region: a BL is a direct CALL. The callee is
                    // NOT part of the trace — the region's BL_CALL codegen
                    // invokes the (already-translated) standalone callee fn
                    // via a patchable `call rel32`, and the trace continues at
                    // the caller's fall-through (ip+4). Requiring the callee
                    // pre-translated means the direct-call slot patches
                    // immediately; an untranslated callee would bounce through
                    // jit_call_helper → INCOMPLETE unwind → dispatcher on
                    // every iteration, which is worse than no region.
                    // BIFROST_NO_CALLREGION=1 restores the M1 abort.
                } else {
                    trace.stop_reason = "bl_untranslated";
                    aborted = true;
                    break;
                }
            }
            if (d.cls == InstClass::BLR && no_callregion_) {
                // BLR (function pointer): the target is dynamic, so no
                // pre-translation check is possible. BLR_CALL codegen routes
                // through jit_call_helper, which runs the whole callee and
                // returns the caller's continuation — correct from inside a
                // region, so BLR is allowed by default (Track 1; the worldgen
                // noise path is blr-heavy). BIFROST_NO_CALLREGION=1 restores
                // the M1 abort.
                trace.stop_reason = "indirect_br";
                aborted = true;
                break;
            }

            // RET: translate it (it ends the block), then end the trace.
            if (d.cls == InstClass::RET) {
                translate_to_ir(tb.ir, d, ip);
                instr_count++;
                tb.ir.count = instr_count;
                trace.stop_reason = "ret";
                trace_ends_after_block = true;
                block_ended = true;
                break;
            }

            // Translate to IR and scan the ops this instruction appended for an
            // interpreter fallback — such an instruction can't live in a region.
            size_t op_start = tb.ir.insts.size();
            bool ends = translate_to_ir(tb.ir, d, ip);
            instr_count++;
            tb.ir.count = instr_count;
            for (size_t i = op_start; i < tb.ir.insts.size(); i++) {
                if (tb.ir.insts[i].op == IROp::CALL_INTERP ||
                    tb.ir.insts[i].op == IROp::SIMD_NATIVE_STRUCT) {
                    // Structured memory helpers return a dynamic PC on a
                    // guest fault and end the Tier-1 block. Regions must
                    // not continue through their fault/signal boundary.
                    trace.stop_reason = tb.ir.insts[i].op == IROp::CALL_INTERP
                        ? "call_interp" : "structured_memory";
                    aborted = true;
                    break;
                }
            }
            if (aborted) break;

            // ── Branch handling ──────────────────────────────────────
            // Conditional branches: the taken target leaves the trace (recorded
            // as a side exit / back-edge), the fall-through (ip + 4) continues.
            // Target = pc + d.imm, mirroring the interpreter and translate_block
            // (ir_translate.cpp Bcond/CBZ/TBZ cases).
            bool is_cond_branch = (d.cls == InstClass::Bcond ||
                                   d.cls == InstClass::CBZ ||
                                   d.cls == InstClass::CBNZ ||
                                   d.cls == InstClass::TBZ ||
                                   d.cls == InstClass::TBNZ);
            if (is_cond_branch) {
                uint64_t target = ip + static_cast<uint64_t>(d.imm);
                uint64_t side_pc = target;
                auto profile = tier2_profiles_by_pc_.find(ip);
                uint64_t taken = 0, fallthrough = 0;
                if (profile != tier2_profiles_by_pc_.end()) {
                    taken = profile->second->taken.load(std::memory_order_relaxed);
                    fallthrough = profile->second->fallthrough.load(std::memory_order_relaxed);
                }
                const uint64_t samples = taken + fallthrough;
                const bool profiled = samples >= 16;
                trace.fully_profiled &= profiled;
                // Keep a head back-edge as the loop terminator. Otherwise
                // linearize a strongly biased taken successor and make the
                // original fall-through the cold exit by inverting the IR.
                const bool follow_taken = profiled && taken * 4 >= samples * 3 &&
                    target != head_pc && (d.cls != InstClass::Bcond || d.cond < 14) &&
                    std::find(visited.begin(), visited.end(), target) == visited.end();
                if (follow_taken) {
                    IRInst& terminal = tb.ir.insts.back();
                    if (terminal.op == IROp::BRCOND) {
                        auto p = terminal.brcond_params(); p.cond ^= 1; p.target = ip + 4;
                        terminal = IRInst::make_brcond(p, ip);
                    } else if (terminal.op == IROp::BRCOND_ZERO) {
                        auto p = terminal.brcond_zero_params(); p.cond ^= 1; p.target = ip + 4;
                        terminal = IRInst::make_brcond_zero(terminal.src1, p, ip);
                    } else {
                        auto p = terminal.brcond_bit_params(); p.cond ^= 1; p.target = ip + 4;
                        terminal = IRInst::make_brcond_bit(terminal.src1, p, ip);
                    }
                    tb.follows_taken = true;
                    tb.hot_successor = target;
                    next_pc = target;
                    side_pc = ip + 4;
                }
                if (profiled) {
                    const uint64_t continued = (follow_taken || target == head_pc)
                        ? taken : fallthrough;
                    trace.path_probability *= static_cast<double>(continued) / samples;
                }
                tb.side_exits.push_back(
                    {side_pc, static_cast<int>(tb.ir.insts.size() - 1)});
                block_ended = true;
                // A conditional branch whose TAKEN target is the head is the
                // loop's back-edge: end the trace HERE (this block's taken edge
                // becomes the region's Lback). Without this, a natural loop
                // whose back-edge is the taken path (GCC emits `b.ne .L3` at
                // the loop bottom) would walk out of the loop on the
                // fall-through and the trace would follow the exit instead.
                if (target == head_pc) {
                    trace.stop_reason = "b_backedge";
                    trace_ends_after_block = true;
                }
                break;
            }
            // Direct jumps carry no side exit when their target is the next
            // collected block. Follow them, including non-contiguous bodies;
            // never follow a cycle that bypasses the region head.
            if (d.cls == InstClass::B) {
                uint64_t target = ip + static_cast<uint64_t>(d.imm);
                tb.side_exits.push_back(
                    {target, static_cast<int>(tb.ir.insts.size() - 1)});
                next_pc = target;
                if (target == head_pc) {
                    trace.stop_reason = "b_backedge";
                    trace_ends_after_block = true;
                } else if (std::find(visited.begin(), visited.end(), target) != visited.end()) {
                    trace.stop_reason = "revisit";
                    trace_ends_after_block = true;
                }
                block_ended = true;
                break;
            }
            // A disabled in-block call can lower to a block-ending jump.
            // Do not mistake its callee target for a straight-line cap.
            if (ends) {
                trace.stop_reason = "unsupported_terminator";
                aborted = true;
                break;
            }
            ip += 4;
        }

        if (aborted) break;
        if (instr_count == 0) break;  // nothing decoded — discard the block
        if (tb.side_exits.empty() && !trace_ends_after_block) {
            // The cap stopped before translating ip. Preserve the exact
            // continuation without pretending a guest branch was executed.
            next_pc = ip;
            emit_brcond_fallthru(tb.ir, next_pc, ip);
            tb.side_exits.push_back({next_pc, static_cast<int>(tb.ir.insts.size() - 1)});
        } else if (next_pc == cur_pc) {
            next_pc = ip + 4;
        }
        // Mirror translate_block: run the IR optimizer on each block BEFORE
        // concatenation. Without it the region IR keeps every
        // STORE_REG→LOAD_REG round-trip that optimize_ir's Pass 1 (store-load
        // forwarding) would collapse into a single vreg — an invariant chain
        // threaded through a guest GPR then stays unhoistable to LICM (the
        // STORE_REG is an arch write, the LOAD_REG reads written state), so
        // the region's preheader can never absorb it. Same guard as
        // translate_block: blocks with BRCOND_SKIP skip optimization (its DCE
        // could erase ops inside the skip region and invalidate the skip's
        // op-count imm). The term op survives optimization, so re-derive the
        // recorded side-exit op index (insts.size()-1) afterwards.
        static bool walker_no_opt_ = (getenv("BIFROST_NO_OPT") != nullptr);
        bool has_brcond_skip = false;
        for (const auto& ir_inst : tb.ir.insts) {
            if (ir_inst.op == IROp::BRCOND_SKIP) {
                has_brcond_skip = true;
                break;
            }
        }
        if (!walker_no_opt_ && !has_brcond_skip) {
            // Force FWD on for region blocks (see optimize_ir's force_fwd):
            // it collapses STORE_REG→LOAD_REG round-trips into single vregs,
            // which is what makes real guest invariant chains LICM-hoistable.
            optimize_ir(tb.ir, /*force_fwd=*/true);
            if (!tb.side_exits.empty()) {
                tb.side_exits[0].second =
                    static_cast<int>(tb.ir.insts.size() - 1);
            }
        }
        // Same per-op contract check as the tier-1 path (jit_translate):
        // runs whether or not the optimizer ran, validating what the
        // region compiler is about to consume.
        if (dbg().ir_validate) validate_ir_block(tb.ir);
        trace.total_insts += static_cast<uint64_t>(instr_count);
        trace.blocks.push_back(std::move(tb));
        if (trace_ended || trace_ends_after_block) {
            trace_ended = true;
            break;
        }
        cur_pc = next_pc;
    }

    // ── Finalize ────────────────────────────────────────────────────
    if (!aborted && !trace_ended) {
        // Exited the outer loop at the block cap.
        trace.stop_reason = "block_cap";
        trace_ended = true;
    }
    // Multi-block traces qualify for a region — and so does a 1-block
    // SELF-LOOP (its cond branch targets itself, stop=b_backedge): it gains
    // the LICM preheader + DCE + const-prop that a plain block can't have,
    // which is Phase 2's "fuse the self-loop". A 1-block LINEAR trace is
    // just the existing block JIT and stays rejected. An aborted trace is
    // never compiled regardless of length.
    bool self_loop_trace = (trace.blocks.size() == 1 &&
                            !trace.blocks.empty() &&
                            trace.blocks[0].side_exits.size() == 1 &&
                            trace.blocks[0].side_exits[0].first == head_pc);
    trace.ok = (!aborted && (trace.blocks.size() >= 2 || self_loop_trace));
    if (trace.ok) {
        tier2_traces.fetch_add(1, std::memory_order_relaxed);
    } else if (!aborted && trace.stop_reason == nullptr) {
        // No specific reason ended a short trace — it was just too short.
        trace.stop_reason = "too_short";
    }

    if (tier2_trace_enabled()) {
        fprintf(stderr, "[tier2] trace pc=0x%llx blocks=%zu insts=%llu ok=%d stop=%s\n",
                static_cast<unsigned long long>(head_pc), trace.blocks.size(),
                static_cast<unsigned long long>(trace.total_insts), static_cast<int>(trace.ok),
                trace.stop_reason ? trace.stop_reason : "-");
        for (size_t i = 0; i < trace.blocks.size(); i++) {
            fprintf(stderr, "[tier2]   block %zu pc=0x%llx insts=%d side_exits=%zu\n",
                    i, static_cast<unsigned long long>(trace.blocks[i].pc),
                    trace.blocks[i].ir.count, trace.blocks[i].side_exits.size());
        }
    }
    return trace;
}

// ─────────────────────────────────────────────────────────────────────────
// Tier-2 Phase 1 step 3 / M1: compile a collected trace as ONE x86 function
// with ONE region-wide register allocation. See the header contract
// (frostjit.hpp:479) and ROADMAP #14 for the full design; the layout:
//
//   prologue                       (push/frame/reg-setup, R10 window if used)
//   body_start:  block0 body        (all body ops via compile_ir_inst)
//                [flag prep][cmc] jcc0 → cold exit 0 / back-edge; [cmc restore]
//                block1 body ... block(N-1) body
//                [flag prep][cmc] jccN → Lback; [cmc restore]
//   L_exit:      clobber_flags; flush_all_vregs; store exit_pc; restore; ret
//   cold exit 0  (restore snapshot 0): [cmc] materialize; flush; store pc; ret
//   ...          cold exit N-2
//   Lback:       (restore last snapshot): [cmc][materialize if loop-carried]
//                flush_all_vregs; jmp body_start
//
// Internal fall-through edges carry the flags in host RFLAGS (no per-edge
// pstate materialize — the win); only the side exits, the cold fall-through
// exit, and the back-edge (when the loop top reads pstate) materialize.
// The back-edge flushes loop-carried state to cpu.regs[]/stack and jumps to
// body_start; block0's body was compiled from a fresh regalloc state, so it
// reloads everything from memory.
//
// Called with blocks_mutex_ held EXCLUSIVE (the region fn lands in code_buf_
// alongside translate_block's output). Returns nullptr if the trace shape
// isn't region-qualified.
uint64_t (*FrostJIT::compile_tier2_region(Emulator& emu, const Tier2Trace& trace,
                                          uint64_t (**chain_out)(CPU*, Emulator*),
                                          Tier2RegionMonitor* monitor_override))(CPU*, Emulator*) {
    (void)emu;
    if (chain_out) *chain_out = nullptr;
    // Call-aware regions (Track 1): a trace may now contain BL_CALL/BLR_CALL
    // body ops. Their codegen invokes guest callee fns (direct `call rel32`
    // for BL, jit_call_helper for BLR) that run with their OWN frames and
    // clobber all caller-saved state, so the region treats a call site as a
    // hard boundary: pins are disabled, LICM stops hoisting arch loads, and
    // the region's per-op regalloc reloads everything after the call (the
    // call's invalidate_all_vregs leaves the same clean state the region body
    // starts from). The region frame is laid out exactly like a standalone
    // block's (push rbx/rbp/r12-r15, mov rbp,rsp), so the BL_CALL
    // callee-completion guard's INCOMPLETE path (`mov rsp,rbp; pop×6; ret`)
    // correctly unwinds a mid-callee return to the C dispatcher.
    if (!code_buf_ || !trace.ok) return nullptr;
    // Chain-skip COMPOSITION (1.5.4-alpha): under BIFROST_CHAIN_SKIP=1 the
    // region allocates the same unified kChainSkipFrameBytes frame as every
    // block, records a chain_entry label after the emu stash (before the R10
    // window load / pin preloads / preheader), and publishes it via
    // *chain_out. A chain edge is a bare jmp carrying the chain ROOT's
    // rbp/rsp, so it enters past the frame allocation and runs the body with
    // vreg slots relative to the root frame — legal because every
    // chain-skip frame has identical layout (vreg ceiling -32512 < 32 KiB,
    // fixed emu slot at -(kChainSkipFrameBytes-8)). Cold dispatches enter at
    // fn, which allocates the unified frame itself. Region exits keep the
    // `mov rsp,rbp; pop×6; ret` shape: with rbp = the root frame they unwind
    // the whole chain back to the dispatcher — exactly what a standalone
    // block's cold exit does when chained over.

    const auto& blocks = trace.blocks;
    const size_t nblk = blocks.size();
    if (nblk < 1) return nullptr;
    if (nblk == 1) {
        // 1-block region = a SELF-LOOP: the single block's cond branch must
        // target the head (itself), giving the region an Lback + LICM
        // preheader. A 1-block LINEAR trace carries no back-edge and gains
        // nothing over the standalone block JIT — reject it.
        const auto& b0 = blocks[0];
        if (b0.side_exits.size() != 1 ||
            b0.side_exits[0].first != trace.head_pc) {
            return nullptr;
        }
    }

    // A region is a linearized path with conditional side exits. Direct
    // jumps may be internal only when their target is the next block.
    for (size_t i = 0; i < nblk; i++) {
        const auto& b = blocks[i];
        if (b.ir.insts.empty() || b.side_exits.size() != 1) return nullptr;
        const int term_idx = static_cast<int>(b.ir.insts.size()) - 1;
        if (b.side_exits[0].second != term_idx) return nullptr;
        const IRInst& term = b.ir.insts[term_idx];
        if (term.op == IROp::BRCOND_FALLTHRU) {
            if (term.fallthru_params().target != b.side_exits[0].first) return nullptr;
            if (i + 1 < nblk && b.side_exits[0].first != blocks[i + 1].pc)
                return nullptr;
        } else if (term.op != IROp::BRCOND && term.op != IROp::BRCOND_ZERO &&
                   term.op != IROp::BRCOND_BIT) {
            return nullptr;
        } else if (i + 1 < nblk &&
                   (b.follows_taken ? b.hot_successor : b.pc + b.ir.count * 4) !=
                       blocks[i + 1].pc) {
            return nullptr;
        }
        if (i + 1 < nblk && b.side_exits[0].first == trace.head_pc) return nullptr;
    }
    // The last block's taken edge → the loop back-edge iff it targets the head.
    bool last_is_backedge = (blocks[nblk - 1].side_exits[0].first == trace.head_pc);
    // LOOP REGIONS ONLY (default, 2026-08-20): a LINEAR region over a
    // walked-out loop body re-pays a full region entry (prologue + cold-exit
    // ret to the dispatcher) every iteration — strictly worse than the
    // chained standalone blocks it replaces. Measured on CoreMark: tier2
    // all-regions 3239 iters/s vs plain 3479 (-7%); loops-only 3602 (+3.5%).
    // Genuine loops whose back-edge is a TAKEN conditional or an
    // unconditional `b head` end their trace with stop=b_backedge and still
    // fuse; only truly linear runs (and walks that exit a loop through an
    // outer edge) are declined. BIFROST_T2_LINEAR=1 restores all-region
    // behavior for bisection.
    static const bool t2_linear_ = (getenv("BIFROST_T2_LINEAR") != nullptr);
    if (!last_is_backedge && !t2_linear_) return nullptr;
    tier2_current_monitor_ = monitor_override;
    if (!monitor_override && tier2_retrains_[trace.head_pc] < 2) {
        auto monitor = std::make_unique<Tier2RegionMonitor>();
        monitor->head = trace.head_pc;
        static std::atomic<uint64_t> next_identity{1};
        monitor->identity = next_identity.fetch_add(1, std::memory_order_relaxed);
        monitor->slot = monitor->identity % CPU::TIER2_FEEDBACK_SLOTS;
        tier2_current_monitor_ = monitor.get();
        tier2_region_monitors_.push_back(std::move(monitor));
    }
    // MT regions fuse a whole loop iteration, then use a revocable edge
    // into the head. This bounds execution and avoids retaining registers
    // or LICM values across an invalidation/scheduling boundary.
    if (mt_active()) last_is_backedge = false;

    // ── Vreg remap + IR concatenation ────────────────────────────────
    // Each block's IR used its own scratch numbering (starts at 33, the
    // walker reset g_alloc per block). Remap block i's scratch vregs into a
    // disjoint range so the whole region shares one regalloc/vreg-slot space
    // (scratch vregs are block-local SSA, so the remap is a pure bijection).
    // base[i] = the offset added to block i's scratch vregs (>= 33).
    std::vector<size_t> base(nblk, 0);
    size_t cur_max = 32;  // highest remapped vreg so far
    for (size_t i = 0; i < nblk; i++) {
        base[i] = cur_max - 32;
        size_t bmax = 32;
        for (const auto& inst : blocks[i].ir.insts) {
            if (inst.dest > bmax) bmax = inst.dest;
            if (inst.src1 > bmax) bmax = inst.src1;
            if (inst.src2 > bmax) bmax = inst.src2;
            if (inst.aux  > bmax) bmax = inst.aux;
        }
        size_t remapped_max = base[i] + bmax;
        if (remapped_max >= 4096) return nullptr;
        cur_max = remapped_max;
    }
    const int region_max_vreg = static_cast<int>(cur_max);

    struct RBlock {
        size_t   start;      // first op index in region_ir
        size_t   term;       // terminating branch op index
        uint64_t side_pc;    // taken (side-exit / back-edge) target
        uint64_t pc;         // block start PC (for exit_pc)
        size_t   inst_count; // ARM instructions in this block
    };
    std::vector<RBlock> rblocks(nblk);
    std::vector<IRInst> region_ir;
    region_ir.reserve(trace.total_insts * 2 + nblk);
    for (size_t i = 0; i < nblk; i++) {
        size_t off = base[i];
        size_t start = region_ir.size();
        for (const auto& inst : blocks[i].ir.insts) {
            IRInst c = inst;
            if (c.dest > 32 && c.dest < 4096) c.dest = static_cast<uint16_t>(c.dest + off);
            if (c.src1 > 32 && c.src1 < 4096) c.src1 = static_cast<uint16_t>(c.src1 + off);
            if (c.src2 > 32 && c.src2 < 4096) c.src2 = static_cast<uint16_t>(c.src2 + off);
            if (c.aux  > 32 && c.aux  < 4096) c.aux  = static_cast<uint16_t>(c.aux  + off);
            region_ir.push_back(c);
        }
        rblocks[i] = {start, region_ir.size() - 1, blocks[i].side_exits[0].first,
                      blocks[i].pc, static_cast<size_t>(blocks[i].ir.count)};
    }

    // Region-wide "contains a call" flag (Track 1 call-aware regions). A
    // BL_CALL/BLR_CALL invokes guest code that can write ANY cpu.regs[] entry
    // and clobber all caller-saved host state, so:
    //   - pinning is disabled (a pin would hold a stale loop-carried value
    //     once the callee overwrites its cpu.regs[] slot — the pin scan below
    //     already refused call-containing regions; this is the same check
    //     hoisted so LICM can consult it too);
    //   - LICM must NOT hoist LOAD_REG of an arch that the REGION never writes
    //     (the callee may write it mid-loop, and a hoisted read would freeze
    //     the pre-call value). Pure scratch IMM/ALU chains stay hoistable —
    //     they live in the region's own frame slots, which the callee (its own
    //     frame) never touches.
    const bool has_call = [&]() {
        for (const IRInst& inst : region_ir) {
            if (inst.op == IROp::SVC || inst.op == IROp::BR ||
                inst.op == IROp::BL_CALL || inst.op == IROp::BLR_CALL ||
                inst.op == IROp::CALL_INTERP || inst.op == IROp::SIMD_NATIVE_EXTRA)
                return true;
        }
        return false;
    }();
    // Chain-skip composition declines CALL-CONTAINING regions: the
    // BL_CALL completion guard's INCOMPLETE unwind (`mov rsp,rbp; pop×6;
    // ret`) assumes rbp is the region's own frame, but a chain-entered
    // region inherits the chain ROOT's rbp — the unwind would discard the
    // live host return addresses (the documented busybox do_wait hang that
    // forced bl_call_disabled_ under chain-skip, jit_translate.cpp:283).
    // Call-free regions compose fully. (Under chain-skip the translator
    // already lowers guest BL to a block-ending BRCOND_FALLTHRU, so BL_CALL
    // cannot appear here anyway; this guards BLR_CALL/CALL_INTERP/SVC.)
    if (chain_skip_enabled() && has_call) return nullptr;

    // ── Phase 2 loop-carried arch GPR pinning (back-edge regions) ─────
    // Runs BEFORE the LICM pass below: LICM consults arch_pin_ to avoid
    // hoisting a LOAD_REG of a pinned arch (the pin already makes the body
    // read free; hoisting it to a preheader slot would be a per-iteration
    // regression). Computing the pins on the pre-LICM IR is safe because LICM
    // only hoists LOAD_REGs of NON-pinned archs, so a pinned arch's read
    // stays in the body and the pin stays live; an unpinned arch whose read
    // gets hoisted simply has no pin. When the region's last block loops back
    // to the head (last_is_backedge), the loop-carried arch GPRs — values READ
    // before they are WRITTEN over the concatenated region IR — are PINNED to
    // fixed callee-saved regs (R12-R15) for the whole loop. The prologue
    // preloads them once (every region entry goes through the prologue; the
    // back-edge jmp to body_start skips it); STORE_REG refreshes the pin; the
    // back-edge (Lback) NO LONGER flushes all vregs, so the pinned values
    // survive the loop in registers — no store→cpu.regs[]→load round trip per
    // iteration. The flush removal is safe because regions write arch GPRs
    // eagerly (STORE_REG) and FP/vector regs eagerly (no fp/vec cache), and
    // every block's scratch vregs are remapped to DISJOINT ranges
    // (compile_tier2_region above), so at the back-edge the only live state
    // is arch GPRs + FP regs — the flush only ever stored dead scratch vregs.
    // The pins are excluded from the allocator pool (pinned_host_regs_), so
    // nothing can claim them mid-loop.
    {
        for (int i = 0; i < 32; i++) arch_pin_[i] = -1;
        pinned_host_regs_ = 0;
        keep_store_dest_ = false;
        if (last_is_backedge && !getenv("BIFROST_NO_PIN")) {
            if (has_call) {
                // Call-aware region: the callee can write any cpu.regs[], so a
                // pin (loop-carried in the register, cpu.regs[] possibly
                // stale/deferred) would hold a stale value after the call.
                // No pins.
            } else {
                uint16_t first_write[32];
                uint16_t first_read[32];
                bool direct_write[32];
                for (int i = 0; i < 32; i++) {
                    first_write[i] = 0xFFFF;
                    first_read[i] = 0xFFFF;
                    direct_write[i] = false;
                }
                size_t n = region_ir.size();
                for (size_t i = 0; i < n; i++) {
                    const IRInst& inst = region_ir[i];
                    if (inst.op == IROp::STORE_REG) {
                        if (inst.dest <= 30 && first_write[inst.dest] == 0xFFFF)
                            first_write[inst.dest] = static_cast<uint16_t>(i);
                        if (inst.src1 <= 30 && first_read[inst.src1] == 0xFFFF)
                            first_read[inst.src1] = static_cast<uint16_t>(i);
                    } else if (inst.op == IROp::LOAD_REG) {
                        if (inst.src1 <= 30 && first_read[inst.src1] == 0xFFFF)
                            first_read[inst.src1] = static_cast<uint16_t>(i);
                    } else if (inst.dest <= 30 &&
                               (inst.op == IROp::CSEL || inst.op == IROp::CSINC ||
                                inst.op == IROp::CSINV || inst.op == IROp::CSNEG ||
                                inst.op == IROp::UBFM || inst.op == IROp::SBFM ||
                                inst.op == IROp::FP_F2I || inst.op == IROp::FP_F2I_FIXED ||
                                inst.op == IROp::FMOV_F2G || inst.op == IROp::FMOV_FHI2G ||
                                inst.op == IROp::SIMD_UMOV ||
                                inst.op == IROp::SIMD_SMOV ||
                                inst.op == IROp::LOAD_MEM || inst.op == IROp::ATOMIC)) {
                        // Direct arch-GPR writers (store_reg_to_vreg /
                        // set_vreg_reg), bypassing STORE_REG — the pin would
                        // hold a stale value. LOAD_MEM/ATOMIC write the
                        // loaded result into the dest vreg directly
                        // (set_vreg_reg(inst.dest, RAX) / store_reg_to_vreg),
                        // so a pinned vreg's value would land in a plain
                        // scratch register that the NEXT iteration's code
                        // freely clobbers — the pin is abandoned and the
                        // loop-carried value is lost (CoreMark region
                        // corruption: wrong CRCs). EXPLICIT list, NOT a
                        // dest<=30 catch-all: branch ops carry a dummy
                        // dest=0 that would wrongly flag x0.
                        direct_write[inst.dest] = true;
                    }
                }
                int n_pins = 0;
                // Loop-carried vregs (read before first write, or loop-
                // invariant reads) get pinned — the value persists across the
                // back-edge in the register. Written-then-read vregs are NOT
                // pinned (measured ~6% slower on bench_mips: the pin occupies
                // a scratch register for the whole loop and the body's
                // LOAD_MEM needs the spare register).
                for (int a = 0; a <= 30 && n_pins < NUM_PIN_REGS; a++) {
                    if (direct_write[a]) continue;
                    if (first_read[a] == 0xFFFF) continue;  // never read — dead
                    if (first_write[a] != 0xFFFF && first_write[a] < first_read[a])
                        continue;  // written before first read — not carried
                    arch_pin_[a] = PIN_REGS[n_pins];
                    pinned_host_regs_ |= (1u << PIN_REGS[n_pins]);
                    n_pins++;
                }
                keep_store_dest_ = true;
            }
        }
    }

    // ── M2 transforms: region DCE + loop-invariant code motion (LICM) ──
    // Run on the concatenated IR after the pin analysis above (which LICM
    // consults to avoid hoisting pinned-arch reads) and BEFORE the liveness/
    // fold analyses below, so those see the transformed IR. Term branches
    // (BRCOND/ZERO/BIT) carry a dummy dest=0 and are never removed; rblocks'
    // start/term indices are rebuilt after removals (side_pc/pc/inst_count are
    // untouched — DCE drops IR ops, never ARM instructions).
    //
    // Pure GPR ops: no flags, no memory, no arch writes beyond dest. The
    // dest>32 scratch guard automatically leaves all FP/SIMD state alone
    // (those write v_lo/v_hi[dest] with dest = the FP reg index 0-31) and all
    // arch writes, flag setters, memory ops, STORE_REG, calls and branches in
    // place.
    auto is_m2_pure_gpr = [](IROp op) -> bool {
        switch (op) {
            case IROp::IMM: case IROp::MOV:
            case IROp::ADD: case IROp::SUB: case IROp::MUL:
            case IROp::AND: case IROp::OR:  case IROp::XOR:
            case IROp::SHL: case IROp::SHR: case IROp::SAR: case IROp::ROR:
            case IROp::NOT: case IROp::NEG: case IROp::SEXT: case IROp::ZEXT:
            case IROp::CLZ: case IROp::CLS: case IROp::RBIT:
            case IROp::REV16: case IROp::REV32: case IROp::REV64:
            case IROp::UBFM: case IROp::SBFM: case IROp::EXTR: case IROp::BFM:
            case IROp::UDIV: case IROp::SDIV:
            case IROp::SMADDL: case IROp::UMADDL:
            case IROp::SMULH: case IROp::UMULH:
            case IROp::SMSUBL: case IROp::UMSUBL:
                return true;
            default:
                return false;
        }
    };

    // (a) Region DCE: drop pure GPR scratch ops whose dest is never read.
    // One pass is sufficient: removing an op only removes a DEFINITION, never
    // a use, so it cannot make another op newly-dead (a leftover dead op
    // merely computes an unused value — harmless).
    if (!getenv("BIFROST_NO_RDCE")) {
        std::vector<uint16_t> uses(4096, 0);
        for (const auto& inst : region_ir) {
            if (inst.op != IROp::LOAD_REG) {
                if (inst.src1 > 31 && inst.src1 < 4096) uses[inst.src1]++;
                if (inst.src2 > 31 && inst.src2 < 4096) uses[inst.src2]++;
            }
            if (inst.aux > 31 && inst.aux < 4096) uses[inst.aux]++;
        }
        region_ir.erase(
            std::remove_if(region_ir.begin(), region_ir.end(),
                           [&](const IRInst& inst) {
                               if (!(inst.dest > 32 && inst.dest < 4096)) return false;
                               if (!is_m2_pure_gpr(inst.op)) return false;
                               return uses[inst.dest] == 0;
                           }),
            region_ir.end());
    }

    // (b) LICM: for back-edge regions only, hoist loop-invariant pure GPR ops
    // and GPR LOAD_REG of never-written, non-pinned arch regs into a preheader
    // emitted once per region entry (the Lback jumps to body_start, past it).
    // All blocks of a back-edge region run every iteration, so "invariant" =
    // sources are never redefined in the region: scratch sources must be
    // defined by a hoisted op (forward pass, defs precede uses), arch sources
    // must be read-only LOAD_REG (sf=0) that is NOT pinned (arch_pin_ was
    // computed above; a pinned read is already free in the body, hoisting it
    // to a preheader slot would be a per-iteration regression). CSEL/CSINC/
    // CSINV/CSNEG read flags and are excluded (flags may be loop-carried).
    // IMM IS hoistable: chains like `add x12,x8,x8,lsl#1` lower to
    // IMM(1);SHL;ADD — excluding the IMM stranded the whole chain in the body
    // (only the bare LOAD_REGs hoisted, which measured ~1% SLOWER on m2loop3
    // because the body then reloaded them from slots). A hoisted IMM emits a
    // dead mov in the preheader (its consumers fold via jit_consts_ or load
    // the slot once per region entry) — negligible. The hoisted results live
    // in their pre-assigned stack slots; block0's body is compiled after a
    // flush+invalidate, so it reloads them from the slots each iteration.
    std::vector<IRInst> preheader;
    if (last_is_backedge && !getenv("BIFROST_NO_LICM")) {
        std::vector<bool> hoisted_vreg(4096, false);
        std::vector<char> hoisted_op(region_ir.size(), 0);
        for (size_t i = 0; i < region_ir.size(); i++) {
            const IRInst& inst = region_ir[i];
            if (!(inst.dest > 32 && inst.dest < 4096)) continue;
            if (inst.op == IROp::CSEL || inst.op == IROp::CSINC ||
                inst.op == IROp::CSINV || inst.op == IROp::CSNEG)
                continue;
            if (inst.op != IROp::LOAD_REG && !is_m2_pure_gpr(inst.op)) continue;
            auto src_invariant = [&](uint16_t s) -> bool {
                if (s == 0) return true;  // no source
                if (s > 32 && s < 4096) return hoisted_vreg[s];
                return false;  // arch/XZR/SP sources on a pure op — never hoist
            };
            if (inst.op == IROp::LOAD_REG) {
                // A register read can reuse a dirty arch-vreg mapping that is
                // not represented by cpu.regs[] yet. Keep it in the body;
                // only pure scratch chains are safe to move to the preheader.
                continue;
            } else if (!src_invariant(inst.src1) ||
                       !src_invariant(inst.src2) ||
                       !src_invariant(inst.aux)) {
                continue;
            }
            hoisted_vreg[inst.dest] = true;
            hoisted_op[i] = 1;
        }
        // Refine: un-hoist a hoisted IMM or LOAD_REG with NO hoisted consumer.
        // A dead preheader mov is harmless, but a hoisted IMM that only fed
        // body ops (e.g. `mov wN,#imm` feeding a STORE_REG) or a hoisted
        // PINNED-arch LOAD_REG with no hoisted chain consumer would make the
        // body consumer load from a slot each iteration (slower than the
        // in-body mov / free pin read). Safe: any hoisted op that consumed
        // the IMM is itself a hoisted consumer, so this only ever un-hoists
        // ops nothing hoisted depends on.
        for (size_t i = 0; i < region_ir.size(); i++) {
            if (!hoisted_op[i]) continue;
            if (region_ir[i].op != IROp::IMM && region_ir[i].op != IROp::LOAD_REG) continue;
            uint16_t d = region_ir[i].dest;
            bool any_hoisted_consumer = false;
            for (size_t j = i + 1; j < region_ir.size(); j++) {
                if (!hoisted_op[j]) continue;
                const IRInst& c = region_ir[j];
                if (c.src1 == d || c.src2 == d || c.aux == d) {
                    any_hoisted_consumer = true;
                    break;
                }
            }
            if (!any_hoisted_consumer) {
                hoisted_op[i] = 0;
                hoisted_vreg[d] = false;
            }
        }
        std::vector<IRInst> body_ir;
        body_ir.reserve(region_ir.size());
        for (size_t i = 0; i < region_ir.size(); i++) {
            if (hoisted_op[i]) preheader.push_back(region_ir[i]);
            else body_ir.push_back(region_ir[i]);
        }
        region_ir.swap(body_ir);
    }

    // Rebuild rblocks start/term over the transformed IR (term ops are never
    // removed, one per block, in order).
    {
        size_t blk_idx = 0, start = 0;
        for (size_t i = 0; i < region_ir.size(); i++) {
            IROp op = region_ir[i].op;
            if (op == IROp::BRCOND || op == IROp::BRCOND_ZERO ||
                op == IROp::BRCOND_BIT || op == IROp::BRCOND_FALLTHRU) {
                rblocks[blk_idx].start = start;
                rblocks[blk_idx].term = i;
                blk_idx++;
                start = i + 1;
            }
        }
    }

    // ── M2 dest-sourcing rename (2026-08-21) ────────────────────────────
    // The ALU/shift emitters have an in-place fast path for dest == src1
    // (compute in src1's register, no alloc, no mov), but translated IR
    // mints a FRESH scratch dest for every op, so the general path
    // (alloc_reg_excluding + `mov d,s1` + op) fires almost always — on the
    // hot core_bench_list region, 64 of 343 dynamic instructions/iteration
    // were pure reg→reg movs, the single largest waste class. When src1 is
    // a SCRATCH vreg whose LAST USE is this op and dest is a scratch with
    // its (unique) def here and no earlier use, rewrite dest := src1 and
    // rename every later operand that referenced dest — the emitters' fast
    // path then computes destructively in src1's register. Safety: scratch
    // vregs are uniquely defined per block and remapped to disjoint ranges
    // (so dest has exactly one def — this op — and no use can precede it),
    // src1 dying here means nothing else reads the destroyed value, and
    // arch vregs (<=32) are excluded so cpu.regs[] homes and pins are
    // untouched. old_dest > 32 also makes operand renames unambiguous
    // against LOAD_REG's arch-index src1.
    if (!getenv("BIFROST_NO_DESTSRC")) {
        const size_t n = region_ir.size();
        auto is_destsrc_op = [](IROp op) -> bool {
            switch (op) {
                case IROp::ADD: case IROp::SUB: case IROp::AND:
                case IROp::OR:  case IROp::XOR: case IROp::MUL:
                case IROp::SHL: case IROp::SHR: case IROp::SAR:
                case IROp::ROR:
                    return true;
                default:
                    return false;
            }
        };
        // Last-use index per vreg and def/use counts (operands: src1 except
        // on LOAD_REG where it is the arch index, plus src2 and aux).
        std::vector<int> last_use(4096, -1);
        std::vector<int> ndefs(4096, 0);
        std::vector<int> first_use_idx(4096, -1);
        for (size_t i = 0; i < n; i++) {
            const IRInst& inst = region_ir[i];
            uint16_t ops[3] = {inst.src1, inst.src2, inst.aux};
            for (int k = 0; k < 3; k++) {
                uint16_t s = ops[k];
                if (s < 33 || s >= 4096) continue;
                if (k == 0 && inst.op == IROp::LOAD_REG) continue;  // arch index
                last_use[s] = static_cast<int>(i);
                if (first_use_idx[s] < 0) first_use_idx[s] = static_cast<int>(i);
            }
            if (inst.dest > 32 && inst.dest < 4096) ndefs[inst.dest]++;
        }
        int renamed = 0;
        for (size_t i = 0; i < n; i++) {
            IRInst& inst = region_ir[i];
            if (!is_destsrc_op(inst.op)) continue;
            if (!(inst.dest > 32 && inst.dest < 4096)) continue;
            if (!(inst.src1 > 32 && inst.src1 < 4096)) continue;
            if (inst.dest == inst.src1) continue;
            if (inst.dest == inst.src2 || inst.dest == inst.aux) continue;
            // dest: uniquely defined here, never read before its def.
            if (ndefs[inst.dest] != 1) continue;
            if (first_use_idx[inst.dest] >= 0 &&
                first_use_idx[inst.dest] < static_cast<int>(i)) continue;
            // src1 must die AT this op (it is consumed destructively).
            if (last_use[inst.src1] != static_cast<int>(i)) continue;
            const uint16_t old_dest = inst.dest;
            inst.dest = inst.src1;
            // Rename all later operand references old_dest → src1.
            for (size_t j = i + 1; j < n; j++) {
                IRInst& later = region_ir[j];
                if (later.op != IROp::LOAD_REG) {
                    if (later.src1 == old_dest) later.src1 = inst.src1;
                }
                if (later.src2 == old_dest) later.src2 = inst.src1;
                if (later.aux  == old_dest) later.aux  = inst.src1;
            }
            // src1 now inherits old_dest's readers: extend its last_use so a
            // LATER dest-sourcing rename cannot destroy the value those
            // redirected readers still need (chained-rename case).
            if (last_use[old_dest] > last_use[inst.src1]) {
                last_use[inst.src1] = last_use[old_dest];
            }
            renamed++;
        }
        if (tier2_trace_enabled() && renamed) {
            fprintf(stderr, "[tier2] destsrc: %d ops rewritten\n", renamed);
        }
    }

    // ── Codegen state reset (mirror translate_block's start) ─────────
    make_writable();
    current_start_pc_ = trace.head_pc;
    code_buf_overflow_ = false;
    call_interp_branch_patches_.clear();
    branch_target_patches_.clear();
    skip_fixups_.clear();
    rax_holds_next_pc_ = false;
    flags_in_host_ = false;
    flags_from_sub_ = false;
    chain_target_pc_ = 0;
    unchainable_end_ = true;  // the region never chains out
    has_selfloop_slot_ = false;
    selfloop_patch_off_ = 0;
    has_taken_chain_slot_ = false;
    taken_chain_patch_off_ = 0;
    taken_chain_target_pc_ = 0;
    pending_flag_mat_.clear();
    chain_entry_off_ = 0;
    num_stack_slots_ = 0;
    vec_cache_reset();  // regions never use the vec/fp cache (memory paths)
    regalloc_stats_reset();
    size_t block_start = code_buf_used_;
    // A region can be smaller than the preceding Tier-1 block. Clearing
    // only its own vregs leaves stale homes above region_max_vreg, then
    // publishing that smaller maximum makes the next block skip them too.
    // Region compilation is rare; reset the entire table, including aborts.
    constexpr int clear_limit = 4096;
    for (int i = 0; i < clear_limit; i++) {
        vreg_home_[i] = -1;
        vreg_dirty_[i] = false;
        vreg_slot_[i] = 0;
        vreg_last_use_[i] = 0;
    }
    for (int i = 0; i < NUM_HOST_REGS; i++) reg_vreg_[i] = -1;
    max_vreg_ = region_max_vreg;
    dirty_host_regs_ = 0;
    // Pre-assign stack slots for every region scratch vreg (fixed frame).
    for (int v = 33; v <= region_max_vreg; v++) {
        vreg_slot_[v] = -8 * (v - 32);
    }
    num_stack_slots_ = region_max_vreg - 32;
    if (num_stack_slots_ < 1) num_stack_slots_ = 1;
    uint32_t stack_bytes = static_cast<uint32_t>(num_stack_slots_ * 8 + 64) & ~15U;

    // ── Liveness over the concatenated region IR (mirror translate_block) ──
    {
        size_t n = region_ir.size();
        vreg_last_use_op_.assign(4096, -1);
        vreg_uses_.assign(4096, {});
        for (size_t i = 0; i < n; i++) {
            const IRInst& inst = region_ir[i];
            if (inst.op != IROp::LOAD_REG) {
                if (inst.src1 > 31 && inst.src1 < 4096) {
                    vreg_last_use_op_[inst.src1] = static_cast<int>(i);
                    vreg_uses_[inst.src1].push_back(static_cast<uint16_t>(i));
                }
                if (inst.src2 > 31 && inst.src2 < 4096) {
                    vreg_last_use_op_[inst.src2] = static_cast<int>(i);
                    vreg_uses_[inst.src2].push_back(static_cast<uint16_t>(i));
                }
            }
            if (inst.aux > 31 && inst.aux < 4096) {
                vreg_last_use_op_[inst.aux] = static_cast<int>(i);
                vreg_uses_[inst.aux].push_back(static_cast<uint16_t>(i));
            }
        }
        kills_per_op_.assign(n, {});
        for (int v = 32; v < 4096; v++) {
            if (vreg_last_use_op_[v] >= 0) {
                kills_per_op_[vreg_last_use_op_[v]].push_back(static_cast<uint16_t>(v));
            }
        }
        // Fold-lookahead (M2 cross-block const-prop — supersedes translate_block's
        // i+1-adjacent rule): an IMM is fold-ahead-skippable when its dest has
        // EXACTLY ONE consumer anywhere later in the region (vreg_uses_ size
        // 1) and that consumer folds the const into an x86 immediate (ALU or
        // shift kind, same guards). The consumer never reads the vreg's host
        // mapping, so the IMM codegen may skip its mov. Safety: vregs are
        // unique per definition (the IMM is the only writer of its dest), the
        // single-use check guarantees the const vreg is dead AT the consumer
        // (vreg_last_use_this_op holds at op j), jit_consts_ is populated by
        // the IMM before any body op (the preheader's clear precedes the body
        // loop and nothing re-defines the dest between i and j), and the ALU
        // kind-1 imm32 sign-extend fit is re-checked at the IMM codegen
        // (emits the mov on mismatch, so a non-folding consumer still finds
        // the vreg mapped). The shift kind-2 fold has no extra guards.
        fold_ahead_kind_.assign(n, 0);
        for (size_t i = 0; i < n; i++) {
            const IRInst& im = region_ir[i];
            if (im.op != IROp::IMM) continue;
            if (!(im.dest > 32 && im.dest < 4096)) continue;
            if (vreg_uses_[im.dest].size() != 1) continue;  // single consumer
            if (vreg_last_use_op_[im.dest] <= static_cast<int>(i)) continue;
            const IRInst& nx = region_ir[vreg_last_use_op_[im.dest]];
            if (nx.src2 != im.dest || nx.dest == nx.src2 || nx.src1 == nx.src2) continue;
            switch (nx.op) {
                case IROp::ADD: case IROp::SUB: case IROp::AND:
                case IROp::OR:  case IROp::XOR:
                    fold_ahead_kind_[i] = 1;
                    break;
                case IROp::SHL: case IROp::SHR: case IROp::SAR: case IROp::ROR:
                    fold_ahead_kind_[i] = 2;
                    break;
                default:
                    break;
            }
        }
    }
    // Region-wide flag-loop-carry: true iff a flag CONSUMER appears before
    // the first flag SETTER over the concatenated IR. If false, the loop
    // back-edge may skip the pstate materialization (the loop top's first
    // flag-setting op re-establishes host flags before any consumer reads
    // them); if true, the back-edge must materialize (the loop top reads
    // pstate as an input — same contract as the cross-block skip).
    bool region_flags_loop_carried = false;
    {
        bool setter_seen = false;
        for (const IRInst& inst : region_ir) {
            bool consumer = false;
            switch (inst.op) {
                case IROp::FP_CMP:
                    consumer = inst.fp_cmp_params().conditional;
                    break;
                case IROp::CSEL: case IROp::CSINC: case IROp::CSINV: case IROp::CSNEG:
                case IROp::ADCS: case IROp::SBCS: case IROp::CCMP:
                case IROp::FP_CSEL: case IROp::BRCOND: case IROp::BRCOND_SKIP:
                    consumer = true;
                    break;
                default:
                    break;
            }
            if (consumer && !setter_seen) {
                region_flags_loop_carried = true;
                break;
            }
            switch (inst.op) {
                case IROp::ADDS: case IROp::SUBS:
                case IROp::ADCS: case IROp::SBCS:
                case IROp::TST: case IROp::TST_ZERO:
                case IROp::CCMP: case IROp::FP_CMP:
                    setter_seen = true;
                    break;
                default:
                    break;
            }
        }
    }

    // ── Prologue (mirror translate_block) ─────────────────────────────
    emit_push(RBX); emit_push(RBP); emit_push(R12);
    emit_push(R13); emit_push(R14); emit_push(R15);
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xE5);  // mov rbp, rsp
    emit_byte(0x48); emit_byte(0x81); emit_byte(0xEC);
    // Chain-skip: allocate the SAME unified 32 KB frame as every block so a
    // chain edge into the region's chain_entry sees an identical layout
    // (vreg slots -8*(v-32) fit either way; emu_slot_off() is mode-aware).
    emit_u32(chain_skip_enabled() ? kChainSkipFrameBytes : stack_bytes);
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xFB);  // mov rbx, rdi (CPU)
    emit_store(RBP, emu_slot_off(), RSI);               // stash Emulator*
    // Chain-skip entry label (mirror translate_block's chain_entry_off_):
    // recorded AFTER the emu stash, BEFORE the R10 window load / pin
    // preloads / preheader. Chain edges bare-jmp here with the ROOT's
    // rbp/rsp/rbx intact; everything from this point on (window load, pins,
    // preheader, body) re-runs per entry exactly like a chained block.
    const size_t region_chain_entry_off = code_buf_used_;
    // Direct-window base into R10 only if some region op touches the window.
    {
        bool uses_window = false;
        for (const auto& inst : region_ir) {
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
    // No vec/fp-cache prologue loads (regions never activate the caches).
    // Phase 2 pin preloads (cpu.regs[a] → pin reg). Emitted after the R10
    // window load but before body_start: every region entry runs the full
    // prologue (there is no chain-entry shortcut for regions), while the
    // back-edge jmp to body_start skips these — so the pins are loop-carried.
    // The compile-time mapping is established here too, so block0's body reads
    // (LOAD_REG/STORE_REG src1) reuse the pin instead of reloading cpu.regs[].
    // Clean: each pin mirrors cpu.regs[a] exactly.
    for (int a = 0; a <= 30; a++) {
        if (arch_pin_[a] >= 0) {
            emit_load_arm(arch_pin_[a], a);
            vreg_home_[a] = arch_pin_[a];
            reg_vreg_[arch_pin_[a]] = a;
            vreg_dirty_[a] = false;
            vreg_last_use_[a] = ++regalloc_lru_counter_;
        }
    }
    // ── M2 preheader (LICM hoisted invariants) ────────────────────────
    // Emitted AFTER the pin preloads (so hoisted LOAD_REG of a pinned arch
    // reuses the pin — a clean reg→reg move) and BEFORE body_start (so the
    // Lback jumps past it). After the preheader the codegen state is
    // flush+invalidate'd: the hoisted results land in their pre-assigned
    // stack slots and block0's body (compiled below from a fresh regalloc)
    // reloads them every iteration. jit_consts_ is populated BEFORE the body
    // loop so body consumers can fold hoisted IMMs (the body loop's clear is
    // moved up here; preheader consts persist into the body).
    jit_consts_.clear();
    if (!preheader.empty()) {
        // Liveness for the preheader's OWN chain. Without this, the
        // preheader compiled with cur_op_index_ = fold_ahead_kind_.size()
        // (out of range — deliberately, so hoisted IMMs never skip their
        // mov) made EVERY vreg look dead to the Belady allocator
        // (next_use_after returned -1 for all, since the real uses lie at
        // indices < the out-of-range cursor). alloc_reg's Belady eviction
        // then scored every occupied reg equal (INT_MAX) and picked the
        // FIRST allocable reg — RAX — evicting a just-loaded operand:
        // H9's `v44 = v41 + v43` loaded v41 into RAX, then ensure_vreg(v43)
        // evicted v41 and loaded v43 into RAX, emitting `mov rax,rax; add
        // rax,rax` → v44 = 2*v43 instead of v41+v43 (m5self's acc came out
        // 0x33333333 short per call). Fix: inject SYNTHETIC preheader use
        // positions (PREHEADER_BASE + k) into vreg_uses_ so next_use_after
        // sees each preheader-defined vreg's real next read, and compile
        // preheader op k with cur_op_index_ = PREHEADER_BASE + k. Fold-ahead
        // stays off (its bounds check `cur_op_index_ < fold_ahead_kind_
        // .size()` is false for the synthetic cursor) and the body's liveness
        // is unaffected (body indices are all < PREHEADER_BASE, so
        // next_use_after in the body still returns the correct next body
        // use; the synthetic high entries are only consulted during
        // preheader compilation, when the body uses lie BELOW the cursor).
        constexpr size_t PREHEADER_BASE = 40000;
        for (size_t k = 0; k < preheader.size(); k++) {
            const IRInst& inst = preheader[k];
            if (inst.src1 > 32 && inst.src1 < 4096)
                vreg_uses_[inst.src1].push_back(static_cast<uint16_t>(PREHEADER_BASE + k));
            if (inst.src2 > 32 && inst.src2 < 4096)
                vreg_uses_[inst.src2].push_back(static_cast<uint16_t>(PREHEADER_BASE + k));
            if (inst.aux > 32 && inst.aux < 4096)
                vreg_uses_[inst.aux].push_back(static_cast<uint16_t>(PREHEADER_BASE + k));
        }
        for (size_t k = 0; k < preheader.size(); k++) {
            cur_op_index_ = PREHEADER_BASE + k;  // real-chain liveness, fold-ahead off
            const IRInst& inst = preheader[k];
            if (compile_ir_inst(inst)) {
                code_buf_used_ = block_start;
                make_executable();
                return nullptr;
            }
            if (inst.op != IROp::IMM) jit_consts_.erase(inst.dest);
        }
        flush_all_vregs();
        invalidate_all_vregs();

        // ── P2-lite: register-resident LICM results across the Lback ─────
        // The hoisted invariants now live in their (just-flushed, current)
        // stack slots and block0's body reloads them on EVERY iteration.
        // Reload the hottest roots into free CALLEE-SAVED registers here —
        // emitted BEFORE body_start, so the Lback jumps past it and the
        // load runs once per region ENTRY — and pin those regs for the
        // body compile so the allocator never evicts them (same mechanism
        // as the arch GPR pins; callee-saved is required because RAX/RCX/
        // RDX/R8/R9/R11 are in the memory ops' MEM_CLOBBER set — the
        // slow-path C helper destroys caller-saved regs mid-body). The
        // keeps are read-only in the body (LICM invariants), so they stay
        // CLEAN: no exit spill, and the kill at their last body use emits
        // nothing. Blocked when has_call (callee clobbers cpu.regs[] and
        // caller-saved state — mirrors the arch-pin policy) and fenced to
        // leave the body ≥3 allocatable regs (the emitter-audit fence: a
        // steered/pinned pool under 3 regs exposes ALU-emitter variant-2).
        if (!getenv("BIFROST_NO_KEEPRES") && last_is_backedge && !has_call) {
            const size_t n_body = region_ir.size();
            struct KeepCand { uint16_t v; int uses; };
            std::vector<KeepCand> cands;
            for (const auto& inst : preheader) {
                const uint16_t v = inst.dest;
                if (!(v > 32 && v < 4096)) continue;
                int u = 0;
                for (uint16_t ui : vreg_uses_[v]) {
                    if (ui < n_body) u++;  // body uses (synthetic preheader
                                           // entries live at PREHEADER_BASE)
                }
                if (u > 0) cands.push_back({v, u});
            }
            std::sort(cands.begin(), cands.end(),
                      [](const KeepCand& a, const KeepCand& b) {
                          return a.uses > b.uses;
                      });
            int n_alloc_pinned = 0;
            for (int r : ALLOC_REGS) {
                if (pinned_host_regs_ & (1u << r)) n_alloc_pinned++;
            }
            const int max_keeps =
                std::min(NUM_PIN_REGS, NUM_ALLOC_REGS - n_alloc_pinned - 3);
            int placed = 0;
            for (const auto& c : cands) {
                if (placed >= max_keeps) break;
                int r = -1;
                for (int pr : PIN_REGS) {
                    if (!(pinned_host_regs_ & (1u << pr))) { r = pr; break; }
                }
                if (r < 0) break;  // all callee-saved regs are arch pins
                emit_load(r, RBP, vreg_slot_[c.v]);
                vreg_home_[c.v] = r;
                reg_vreg_[r] = c.v;
                vreg_dirty_[c.v] = false;  // read-only: slot stays current
                vreg_last_use_[c.v] = ++regalloc_lru_counter_;
                pinned_host_regs_ |= (1u << r);
                placed++;
            }
            if (tier2_trace_enabled()) {
                fprintf(stderr, "[tier2] keepres: %d candidates, %d pinned\n",
                        static_cast<int>(cands.size()), placed);
            }
        }
    }
    // One CPU-local flag-neutral store per completed hot iteration. No
    // shared atomic increment, register clobber, or per-entry flag save.
    auto emit_completion_credit = [&] {
        if (!tier2_current_monitor_) return;
        const size_t offset = offsetof(CPU, jit_tier2_feedback) +
            tier2_current_monitor_->slot * sizeof(CPU::Tier2FeedbackSlot) +
            offsetof(CPU::Tier2FeedbackSlot, credit);
        emit_byte(0xC7); emit_byte(0x83); // mov dword [rbx+disp32],1
        emit_u32(static_cast<uint32_t>(offset)); emit_u32(1);
    };
    auto emit_trial_backedge = [&] {
        if (!tier2_sampling_compile_) return;
        // Preserve the hot path's host flags and codegen flag state. The
        // quota exit flushes loop-carried pins before returning to the timer.
        const bool saved_host = flags_in_host_, saved_sub = flags_from_sub_;
        const uint16_t saved_dirty = dirty_host_regs_;
        int saved_regs[16]; memcpy(saved_regs, reg_vreg_, sizeof(saved_regs));
        std::vector<int> saved_homes(vreg_home_, vreg_home_ + 4096);
        std::vector<bool> saved_vdirty(vreg_dirty_, vreg_dirty_ + 4096);
        emit_push(RCX);
        emit_load(RCX, CPU_REG, offsetof(CPU, jit_tier2_trial));
        emit_byte(0xE3); emit_byte(15); // inactive: skip flag save/quota check
        emit_byte(0x9C);
        emit_byte(0x83); emit_byte(0xAB);
        emit_u32(offsetof(CPU, jit_tier2_trial_remaining)); emit_byte(1);
        const size_t quota = emit_jcc_rel32_placeholder(4);
        emit_byte(0x9D);
        emit_pop(RCX);
        const size_t hot = emit_jmp_rel32_placeholder();
        const size_t cold = code_buf_used_;
        emit_byte(0x9D); emit_pop(RCX);
        materialize_flags_to_pstate();
        flush_all_vregs_keep();
        for (int a = 0; a <= 30; ++a)
            if (arch_pin_[a] >= 0) emit_store_arm(a, arch_pin_[a]);
        emit_mov_imm_to_rax(trace.head_pc);
        emit_store(CPU_REG, PC_OFF, RAX);
        emit_mov_reg(RDI, CPU_REG); emit_load(RSI, RBP, emu_slot_off());
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);
        emit_pop(R15); emit_pop(R14); emit_pop(R13);
        emit_pop(R12); emit_pop(RBP); emit_pop(RBX); emit_ret();
        patch_jcc_rel32(quota, static_cast<int32_t>(cold - (quota + 6)));
        patch_jmp_rel32(hot, static_cast<int32_t>(code_buf_used_ - (hot + 5)));
        flags_in_host_ = saved_host; flags_from_sub_ = saved_sub;
        dirty_host_regs_ = saved_dirty;
        memcpy(reg_vreg_, saved_regs, sizeof(saved_regs));
        for (unsigned v = 0; v < 4096; ++v) {
            vreg_home_[v] = saved_homes[v]; vreg_dirty_[v] = saved_vdirty[v];
        }
    };
    size_t body_start = code_buf_used_;

    // ── Compile the region body ───────────────────────────────────────
    // Body ops go through the normal compile_ir_inst path (with the per-op
    // kill/dead-scratch-drop, exactly like translate_block's compile loop).
    // The terminating conditional branch of each block is handled MANUALLY
    // (no block-ending epilogue, no chain slots, no dispatcher return): the
    // not-taken path falls through inline into the next block (flags stay in
    // host RFLAGS), and the taken path is a forward rel32 patched at the end
    // to a deferred cold exit (or to Lback for the last block).
    struct Snapshot {
        RegionSnapshot rs;
        size_t jcc_patch;
        bool   need_cmc;
        uint64_t side_pc;
    };
    std::vector<Snapshot> cold;  // per non-last block, in order
    RegionSnapshot back_snap;
    bool has_back_snap = false;
    size_t last_jcc = 0;         // last block's taken JCC (patched → Lback)
    bool last_term_uncond = false;  // last block ends in an unconditional `b`
    bool skip_exit_sections = false;  // uncond back-edge: L_exit/Lback dead
    size_t blk = 0;
    for (size_t i = 0; i < region_ir.size(); i++) {
        while (blk < nblk && i > rblocks[blk].term) blk++;
        const IRInst& inst = region_ir[i];
        cur_op_index_ = i;
        if (i == rblocks[blk].start) emit_tier2_work_probe(rblocks[blk].pc, rblocks[blk].inst_count);

        if (i == rblocks[blk].term) {
            // ── Terminating conditional branch (manual) ─────────────
            // NOTE: BRCOND_ZERO/BRCOND_BIT skip the pushfq/popfq the
            // single-block codegen wraps around their test/bt. clobber_flags()
            // has already materialized pending host flags to pstate and set
            // flags_in_host_ = false, so nothing in the region reads host
            // RFLAGS after the test — the next inline block body reloads
            // flags from pstate when it needs them.
            size_t jcc = 0;
            bool need_cmc = false;
            switch (inst.op) {
                case IROp::BRCOND: {
                    const BrCondParams bp = inst.brcond_params();
                    if (!flags_in_host_) {
                        // FLAGS3 only: the loader clobbers RAX/RCX/RDX
                        // (RDX-only C^from_sub extraction; see x86_backend.cpp).
                        constexpr uint16_t FLAGS3 =
                            (1u << RAX) | (1u << RCX) | (1u << RDX);
                        flush_dirty_host_regs(FLAGS3);
                        flush_scratch_host_regs(FLAGS3);
                        emit_load_flags_from_pstate();
                        emit_normalize_cf_to_sub_convention();
                        invalidate_host_regs(FLAGS3);
                        flags_from_sub_ = true;  // CF is now in SUB convention
                        flags_in_host_ = true;
                    }
                    uint8_t cc = resolve_arm_cond_with_carry(bp.cond, need_cmc);
                    if (need_cmc) emit_byte(0xF5);  // cmc — invert CF for HI/LS
                    jcc = emit_jcc_rel32_placeholder(cc);
                    break;
                }
                case IROp::BRCOND_ZERO: {
                    const BrCondZeroParams bp = inst.brcond_zero_params();
                    clobber_flags();
                    if (reg_vreg_[RAX] >= 0) drop_vreg(reg_vreg_[RAX]);
                    int s1 = ensure_vreg(inst.src1, RAX);
                    if (s1 != RAX) emit_mov_reg(RAX, s1);
                    if (reg_vreg_[RAX] >= 0) drop_vreg(reg_vreg_[RAX]);
                    // W-form tests only the low 32 bits (see the matching
                    // fix in jit_codegen_branch.cpp) — mask dirty uppers.
                    if (!bp.sf) {
                        emit_byte(0x89); emit_byte(0xC0);  // mov eax, eax
                    }
                    emit_test_reg(RAX, RAX);
                    uint8_t cc = (bp.cond == 0) ? 4 /*JE*/ : 5 /*JNE*/;
                    jcc = emit_jcc_rel32_placeholder(cc);
                    break;
                }
                case IROp::BRCOND_BIT: {
                    const BrCondBitParams bp = inst.brcond_bit_params();
                    clobber_flags();
                    if (reg_vreg_[RAX] >= 0) drop_vreg(reg_vreg_[RAX]);
                    int s1 = ensure_vreg(inst.src1, RAX);
                    if (s1 != RAX) emit_mov_reg(RAX, s1);
                    if (reg_vreg_[RAX] >= 0) drop_vreg(reg_vreg_[RAX]);
                    emit_byte(rex(true, false, false, RAX >= 8));
                    emit_byte(0x0F); emit_byte(0xBA);
                    emit_byte(modrm(3, 5, RAX & 7));
                    emit_byte(bp.bit);  // bit number
                    uint8_t cc = (bp.cond == 0) ? 3 /*JNC*/ : 2 /*JC*/;
                    jcc = emit_jcc_rel32_placeholder(cc);
                    break;
                }
                case IROp::BRCOND_FALLTHRU: {
                    // An internal direct edge is already laid out inline.
                    // Keep register/flag state live across this boundary.
                    if (blk + 1 != nblk) {
                        blk++;
                        continue;
                    }
                    last_term_uncond = true;
                    if (last_is_backedge) {
                        // The back-edge is a PLAIN jmp (no JCC): emit it here,
                        // right after the last body op, and skip the L_exit /
                        // Lback sections below (dead code — nothing falls into
                        // them). Materialize pstate first iff the loop top
                        // reads it (mirror the cond-back-edge Lback).
                        if (region_flags_loop_carried) {
                            materialize_flags_to_pstate();
                        }
                        emit_completion_credit();
                        emit_trial_backedge();
                        int32_t rel = static_cast<int32_t>(
                            body_start - (code_buf_used_ + 5));
                        emit_byte(0xE9);
                        emit_u32(static_cast<uint32_t>(rel));  // jmp body_start
                        skip_exit_sections = true;
                    }
                    blk++;
                    continue;
                }
                default:
                    code_buf_used_ = block_start;
                    make_executable();
                    return nullptr;
            }
            // Snapshot the codegen state AT THE JCC (pre fall-through cmc
            // restore) so the deferred taken-edge epilogue emits as-of here.
            RegionSnapshot snap;
            snap.flags_in_host_ = flags_in_host_;
            snap.flags_from_sub_ = flags_from_sub_;
            snap.need_cmc_ = need_cmc;
            snap.dirty_host_regs_ = dirty_host_regs_;
            memcpy(snap.reg_vreg_, reg_vreg_, sizeof(reg_vreg_));
            int n = max_vreg_ + 1;
            if (n > 4096) n = 4096;
            snap.vreg_home_.resize(static_cast<size_t>(n));
            snap.vreg_dirty_.resize(static_cast<size_t>(n));
            for (int k = 0; k < n; k++) {
                snap.vreg_home_[k] = vreg_home_[k];
                snap.vreg_dirty_[k] = vreg_dirty_[k] ? 1 : 0;
            }
            if (blk + 1 < nblk) {
                // Non-last block: the taken edge is an ordinary cold exit.
                cold.push_back({std::move(snap), jcc, need_cmc, rblocks[blk].side_pc});
            } else if (last_is_backedge) {
                // Last block, back-edge: the taken edge loops to the head.
                last_jcc = jcc;
                back_snap = std::move(snap);
                has_back_snap = true;
            } else {
                // Last block, no back-edge: its taken edge is a cold exit too.
                cold.push_back({std::move(snap), jcc, need_cmc, rblocks[blk].side_pc});
            }
            // Fall-through: restore CF so the next inline block body reads the
            // original carry (the branch's cmc was only for its own JCC).
            if (need_cmc) emit_byte(0xF5);
            // Free host regs of vregs whose last use was the term op.
            if (i < kills_per_op_.size()) {
                for (uint16_t v : kills_per_op_[i]) {
                    if (v != inst.dest) kill_vreg(v);
                }
            }
            blk++;
            continue;
        }

        // ── Body op (normal compile path) ───────────────────────────
        if (compile_ir_inst(inst)) {
            // A body op ended the block. The walker guarantees this never
            // happens (CALL_INTERP / SVC / BR abort the trace; BL/BLR fuse as
            // BL_CALL/BLR_CALL which return false and do not end the block),
            // so this is defensive only.
            code_buf_used_ = block_start;
            make_executable();
            return nullptr;
        }
        if (inst.op != IROp::IMM) jit_consts_.erase(inst.dest);
        if (i < kills_per_op_.size()) {
            for (uint16_t v : kills_per_op_[i]) {
                if (v != inst.dest) kill_vreg(v);
            }
        }
        if (inst.dest > 32 && inst.dest < 4096 &&
            vreg_last_use_op_[inst.dest] <= static_cast<int>(i)) {
            kill_vreg(inst.dest);
        }
    }

    // ── Cold fall-through exit (L_exit), live state ─────────────────
    // The last block's not-taken path falls through here. exit_pc is the
    // guest PC right after the last block (the walker stopped there) — or
    // the unconditional branch target when the last block is `b target`.
    // SKIPPED for an unconditional back-edge (its jmp to body_start was
    // already emitted inline at the term; nothing falls through here).
    // Flush EVERY pinned arch vreg at every exit, not just the ones dirty
    // at the snapshot: a pin can be loop-carried-deferred (block 1 — or a
    // prior Lback iteration — wrote it via STORE_REG into its pin register),
    // so a snapshot "clean" pin may still hold the current value ONLY in the
    // register while cpu.regs[] is stale. A cold exit taken at a block whose
    // IR never writes the pin would otherwise return the stale cpu.regs[]
    // value to the caller (CoreMark crclist: x2 was deferred in block 1,
    // then the "found" cold exit at block 0 returned the pre-loop pointer →
    // wrong list navigation → wrong CRC). Over-flushing is safe — pins always
    // hold the current arch value at runtime.
    auto emit_flush_all_pins = [&]() {
        for (int a = 0; a <= 30; a++) {
            if (arch_pin_[a] >= 0) emit_store_arm(a, arch_pin_[a]);
        }
    };
    // ── Shared exit tail (Track 3: cold-exit dedup) ──────────────────
    // Every cold exit ends with the SAME fixed sequence (store cpu.pc from
    // RAX, rdi=cpu, rsi=emu from the frame slot, frame teardown, ret) —
    // ~33 bytes duplicated per edge. Each exit now ends with `movabs rax,
    // exit_pc; jmp L_shared_exit` (5 bytes) and ONE copy of the tail is
    // emitted after all exits. Only the per-edge VARIABLE part (cmc /
    // flag materialize / flush of that branch point's dirty vregs / pin
    // stores) remains inline — it differs per edge and cannot be shared
    // without runtime metadata. Cold exits are rare (taken edges leaving
    // the trace), so the extra jmp costs nothing measurable; the win is
    // region bytes + I-cache footprint (an 11-block region saves ~300 B).
    std::vector<size_t> shared_exit_patch_offs;
    auto emit_shared_exit_jmp = [&]() {
        size_t p = emit_jmp_rel32_placeholder();
        shared_exit_patch_offs.push_back(p);
    };
    // Region exits use revocable data cells in both modes. Untracked target
    // constants can execute stale guest instructions after invalidation.
    static const bool no_exitchain_ = (getenv("BIFROST_NO_EXITCHAIN") != nullptr);
    const bool exitchain_ok = !no_exitchain_ && !dbg().jit_verify;
    std::vector<size_t> cell_exit_patches;
    auto emit_chainout_exit = [&](uint64_t pc) -> bool {
        if (pc == trace.head_pc) emit_completion_credit();
        emit_mov_imm_to_rax(pc);
        emit_store(CPU_REG, PC_OFF, RAX);
        if (!mt_active()) {
            if (!exitchain_ok || dbg().no_chain || !lookup_only_locked(pc)) return false;
            MtEdge* edge = create_chain_edge(pc, true, code_buf_used_);
            emit_mov_imm_to_rax(pc);
            emit_store(CPU_REG, PC_OFF, RAX);
            emit_mov_imm64(R11, reinterpret_cast<uint64_t>(&edge->target));
            cell_exit_patches.push_back(emit_jmp_rel32_placeholder());
            return true;
        }
        emit_mov_imm_to_rax(pc);
        emit_store(CPU_REG, PC_OFF, RAX);
        emit_mov_reg(RDI, CPU_REG);
        emit_load(RSI, RBP, emu_slot_off());
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);
        emit_pop(R15); emit_pop(R14); emit_pop(R13);
        emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
        if (exitchain_ok) emit_mt_chain_slot(pc, true);
        emit_ret();
        return true;
    };
    if (!skip_exit_sections) {
        uint64_t exit_pc = rblocks[nblk - 1].pc + rblocks[nblk - 1].inst_count * 4;
        if (blocks.back().follows_taken) exit_pc = blocks.back().hot_successor;
        if (last_term_uncond) exit_pc = rblocks[nblk - 1].side_pc;  // the B target
        clobber_flags();
        flush_all_vregs();
        emit_flush_all_pins();
        if (exit_pc != trace.head_pc) emit_completion_credit();
        if (!emit_chainout_exit(exit_pc)) {
            emit_mov_imm_to_rax(exit_pc);
            emit_shared_exit_jmp();
        }
    }

    // ── Deferred side-exit epilogues (one per non-last block) ───────
    // Restore each block's branch-point codegen state, then emit the
    // materialize / flush / pc-store with that state. At runtime the JCC
    // jumps straight here, so the registers still hold the branch-era
    // values; the flush stores exactly those to cpu.regs[]/stack. Each cold
    // exit returns to the C dispatcher (ret) with RAX = the side-exit PC.
    std::vector<size_t> cold_off;
    cold_off.reserve(cold.size());
    for (const auto& c : cold) {
        cold_off.push_back(code_buf_used_);
        const RegionSnapshot& s = c.rs;
        flags_in_host_ = s.flags_in_host_;
        flags_from_sub_ = s.flags_from_sub_;
        dirty_host_regs_ = s.dirty_host_regs_;
        memcpy(reg_vreg_, s.reg_vreg_, sizeof(reg_vreg_));
        for (size_t k = 0; k < s.vreg_home_.size(); k++) {
            vreg_home_[k] = s.vreg_home_[k];
            vreg_dirty_[k] = s.vreg_dirty_[k] != 0;
        }
        if (s.need_cmc_) emit_byte(0xF5);  // restore CF for the materialize
        materialize_flags_to_pstate();
        flush_all_vregs();
        emit_flush_all_pins();
        if (tier2_current_monitor_ && c.side_pc != trace.head_pc) {
            emit_mov_imm_to_rax(c.side_pc);
            emit_store(CPU_REG, PC_OFF, RAX);
            emit_mov_reg(RDI, CPU_REG);
            emit_load(RSI, RBP, emu_slot_off());
            emit_mov_imm64(RDX, reinterpret_cast<uint64_t>(tier2_current_monitor_));
            emit_mov_imm64(R11, reinterpret_cast<uint64_t>(&tier2_feedback_exit));
            emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);
            emit_pop(R15); emit_pop(R14); emit_pop(R13);
            emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
            emit_byte(0x41); emit_byte(0xFF); emit_byte(0xE3); // tail jmp r11
        } else if (!emit_chainout_exit(c.side_pc)) {
            emit_mov_imm_to_rax(c.side_pc);
            emit_shared_exit_jmp();
        }
    }

    if (!cell_exit_patches.empty()) {
        const size_t tail = code_buf_used_;
        emit_mov_reg(RDI, CPU_REG);
        emit_load(RSI, RBP, emu_slot_off());
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);
        emit_pop(R15); emit_pop(R14); emit_pop(R13);
        emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
        // Full-frame targets, including chain-skip. Honor stop sentinels
        // and the dispatch lease before dereferencing the revocable cell.
        emit_byte(0x48); emit_byte(0x3B); emit_byte(0x87);
        emit_u32(offsetof(CPU, jit_stop_pc));
        const size_t stop = emit_jcc_rel32_placeholder(4);
        emit_byte(0x83); emit_byte(0xAF);
        emit_u32(offsetof(CPU, jit_chain_budget)); emit_byte(1);
        const size_t budget = emit_jcc_rel32_placeholder(6);
        emit_load(R11, R11, 0);
        emit_byte(0x4D); emit_byte(0x85); emit_byte(0xDB);
        const size_t empty = emit_jcc_rel32_placeholder(4);
        emit_byte(0x41); emit_byte(0xFF); emit_byte(0xE3);
        for (size_t p : {stop, budget, empty})
            patch_jcc_rel32(p, static_cast<int32_t>(code_buf_used_ - (p + 6)));
        emit_ret();
        for (size_t p : cell_exit_patches)
            patch_jmp_rel32(p, static_cast<int32_t>(tail - (p + 5)));
    }

    // ── Emit the shared exit tail + patch every exit's jmp ──────────
    // Skipped when no exits exist (1-block uncond-back-edge region: the
    // term's inline jmp to body_start is the only way out).
    if (!shared_exit_patch_offs.empty()) {
        size_t tail_off = code_buf_used_;
        emit_store(CPU_REG, PC_OFF, RAX);     // cpu.pc = rax (exit pc)
        emit_mov_reg(RDI, CPU_REG);           // mov rdi, rbx
        emit_load(RSI, RBP, emu_slot_off());  // rsi = emu
        emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);  // mov rsp, rbp
        emit_pop(R15); emit_pop(R14); emit_pop(R13);
        emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
        emit_ret();
        for (size_t p : shared_exit_patch_offs)
            patch_jmp_rel32(p, static_cast<int32_t>(tail_off - (p + 5)));
    }

    // ── Loop back-edge (Lback): the last block's taken edge ─────────
    // Only emitted when the last block's taken target is the head (a hot
    // loop) AND the last block is a CONDITIONAL branch (an unconditional
    // back-edge emitted its jmp inline at the term, skip_exit_sections):
    // flush loop-carried state to cpu.regs[]/stack, then jmp to the
    // region body start. block0's body was compiled with a fresh regalloc
    // state (nothing mapped), so it reloads arch vregs from cpu.regs[] and
    // scratch vregs from their stack slots — the flush makes exactly that
    // state current. Materialize pstate on the back-edge only when the loop
    // top reads it.
    size_t lback_off = code_buf_used_;
    if (!skip_exit_sections && last_is_backedge && has_back_snap) {
        const RegionSnapshot& s = back_snap;
        flags_in_host_ = s.flags_in_host_;
        flags_from_sub_ = s.flags_from_sub_;
        dirty_host_regs_ = s.dirty_host_regs_;
        memcpy(reg_vreg_, s.reg_vreg_, sizeof(reg_vreg_));
        for (size_t k = 0; k < s.vreg_home_.size(); k++) {
            vreg_home_[k] = s.vreg_home_[k];
            vreg_dirty_[k] = s.vreg_dirty_[k] != 0;
        }
        if (region_flags_loop_carried) {
            if (s.need_cmc_) emit_byte(0xF5);
            materialize_flags_to_pstate();
        }
        // NO flush_all_vregs() here (Phase 2): regions write arch GPRs
        // eagerly (STORE_REG) and FP/vector regs eagerly (no fp/vec cache),
        // and each block's scratch vregs occupy DISJOINT remapped ranges, so
        // at the back-edge the only live values are the arch GPRs (pins +
        // cpu.regs[] always current) — the flush only ever stored dead
        // scratch vregs. Pinned arch vregs survive in R12-R15 and block0's
        // body (compiled with the pin mappings pre-established) reuses them.
    }
    if (!skip_exit_sections && last_is_backedge) {
        emit_completion_credit();
        emit_trial_backedge();
        int32_t rel = static_cast<int32_t>(body_start - (code_buf_used_ + 5));
        emit_byte(0xE9);
        emit_u32(static_cast<uint32_t>(rel));  // jmp body_start
    }

    // Patch actual conditional exits, not block indices: internal direct
    // edges have neither a JCC nor a deferred cold snapshot.
    for (size_t k = 0; k < cold.size(); k++) {
        const size_t jcc = cold[k].jcc_patch;
        patch_jcc_rel32(jcc, static_cast<int32_t>(cold_off[k] - (jcc + 6)));
    }
    if (last_is_backedge && !last_term_uncond) {
        patch_jcc_rel32(last_jcc, static_cast<int32_t>(lback_off - (last_jcc + 6)));
    }

    // ── Publish the region ───────────────────────────────────────────
    size_t size = code_buf_used_ - block_start;
    make_executable();
    prev_max_vreg_ = max_vreg_;  // next translate_block's clear_limit (jit_translate.cpp:326)
    auto fn = reinterpret_cast<uint64_t (*)(CPU*, Emulator*)>(code_buf_ + block_start);
    if (code_buf_overflow_ || size == 0) return nullptr;
    if (chain_out && chain_skip_enabled())
        *chain_out = reinterpret_cast<uint64_t (*)(CPU*, Emulator*)>(
            code_buf_ + region_chain_entry_off);
    // TEMP diagnostic (BIFROST_T2_REGIONDUMP): hexdump the published region
    // for offline objdump — same gate name the P2 review session used.
    if (getenv("BIFROST_T2_REGIONDUMP")) {
        fprintf(stderr, "[t2dump] pc=0x%llx fn=%p bytes=%zu body=%zu\n",
                static_cast<unsigned long long>(trace.head_pc),
                reinterpret_cast<void*>(code_buf_ + block_start), size,
                body_start - block_start);
        for (size_t k = 0; k < size; k++) {
            fprintf(stderr, "%02x", code_buf_[block_start + k]);
            if ((k & 31) == 31) fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
    }
    if (tier2_trace_enabled()) {
        fprintf(stderr,
                "[tier2] region pc=0x%llx blocks=%zu insts=%llu bytes=%zu back_flags_carried=%d\n",
                static_cast<unsigned long long>(trace.head_pc), nblk,
                static_cast<unsigned long long>(trace.total_insts), size,
                static_cast<int>(region_flags_loop_carried));
    }
    return fn;
}

// ─────────────────────────────────────────────────────────────────────────
// Tier-2 Phase 1 step 4: IN-CODE hot-head firing.
//
// The dispatch-side counters (run_block slow path / lookup_call_target) can
// never fire on real workloads: hot loops run as chained jmp sequences (or
// inside jit_call_helper's dispatch loop), so only the loop's entry block
// reaches the dispatcher a couple of times and its inline-cache slot then
// self-pins. Block prologues therefore carry an execution counter that
// counts EVERY entry — cold dispatch AND chain edges — and once it crosses
// tier2_hits_threshold() calls tier2_fire_stub() from inside the running
// block. This compiles the head's trace as a region and force-patches the
// loop's back-edge taken-chain slot so the NEXT iteration jumps into the
// region instead of the old per-block functions.
void tier2_fire_stub(Emulator* emu, uint64_t pc, size_t site) {
    if (!emu) return;
    FrostJIT* jit = emu->jit();
    if (!jit) return;
    jit->tier2_fire_region(*emu, pc, site);
}

void FrostJIT::tier2_counter_disable(size_t counter_off, size_t counter_len, bool legacy_transition) {
    if (!counter_off || counter_len < 6) return;
    auto mt = mt_counter_sites_.find(counter_off);
    if (mt != mt_counter_sites_.end()) {
        mt->second->enabled.store(0, std::memory_order_release);
        return;
    }
    if (mt_active() && !legacy_transition) return;
    // Only the single running vCPU may replace this legacy instruction
    // sequence. The MT transition disables it before a worker is launched.
    size_t off = counter_off;
    if (off + 6 > CODE_BUF_SIZE) return;
    const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(counter_len) - 5);
    code_buf_[off] = 0xE9;
    memcpy(code_buf_ + off + 1, &rel, 4);
    std::atomic_thread_fence(std::memory_order_release);
}

void FrostJIT::tier2_fire_region(Emulator& emu, uint64_t pc, size_t site) {
    // Runs from INSIDE a generated block (prologue fire path). Never let an
    // exception cross the JIT frame boundary (std::terminate).
    try {
        // W^X bail: the in-code counter writes the code page (a non-W^X RWX
        // buffer), and the compile here would mprotect the page we're
        // currently executing from. Regions only exist in shared-JIT mode.
        if (wex_enabled_ || !tier2_enabled()) return;
        std::unique_lock<std::shared_mutex> lock(blocks_mutex_);
        auto it = blocks_.find(pc);
        if (it == blocks_.end()) {
            return;
        }
        if (site && it->second.tier2_counter_off != site) return; // retired notification
        BlockEntry old_entry = it->second;
        if (old_entry.tier2_hot_logged) {
            return;
        }
        Tier2Trace trace = collect_tier2_trace(emu, pc);
        // Samples lag the entry counter by one iteration. If a constituent
        // is still cold, retry at most three hotness windows before declining.
        if (!trace.fully_profiled && ++it->second.tier2_attempts < 3) {
            auto mt = mt_counter_sites_.find(site);
            if (mt != mt_counter_sites_.end())
                mt->second->hits.store(0, std::memory_order_relaxed);
            else if (site && old_entry.fn) {
                const uint32_t zero = 0;
                std::memcpy(reinterpret_cast<uint8_t*>(old_entry.fn) - 8, &zero, sizeof(zero));
            }
            tier2_profile_retries.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        it->second.tier2_hot_logged = true;
        tier2_counter_disable(it->second.tier2_counter_off, it->second.tier2_counter_len);
        tier2_hot_heads.fetch_add(1, std::memory_order_relaxed);
        // Require a likely complete path and actual boundary savings. MT
        // self-loops cannot retain pins/LICM across iterations and fuse no edges.
        if (!trace.fully_profiled || trace.path_probability < 0.75 ||
            (mt_active() && trace.blocks.size() < 2)) {
            tier2_profit_rejections.fetch_add(1, std::memory_order_relaxed);
            if (tier2_trace_enabled())
                std::fprintf(stderr, "[tier2] decline pc=0x%llx profiled=%d survival=%.3f blocks=%zu\n",
                    (unsigned long long)pc, trace.fully_profiled, trace.path_probability, trace.blocks.size());
            return;
        }
        // Bound total speculative compiler work and permanent code growth.
        // These are measured budgets, not a claim of predicted speedup.
        if (tier2_retrains_[pc] >= 2 ||
            tier2_code_bytes.load(std::memory_order_relaxed) >= 32ull * 1024 * 1024 ||
            tier2_compile_ns.load(std::memory_order_relaxed) >= 2000000000ull) {
            tier2_profit_rejections.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Sampling compares exact existing Tier-1 block boundaries.
        if (trace.blocks.size() < 2) {
            tier2_profit_rejections.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::vector<std::pair<uint64_t, uint64_t (*)(CPU*, Emulator*)>> baseline_graph;
        uint64_t trial_lo = UINT64_MAX, trial_hi = 0;
        for (const auto& b : trace.blocks) {
            auto baseline = blocks_.find(b.pc);
            if (baseline == blocks_.end() || !baseline->second.fn || baseline->second.is_region ||
                baseline->second.has_call || baseline->second.has_svc || baseline->second.has_selfloop_slot ||
                baseline->second.instr_count != b.ir.count) {
                tier2_profit_rejections.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            baseline_graph.push_back({b.pc, baseline->second.fn});
            trial_lo = std::min(trial_lo, b.pc);
            trial_hi = std::max<uint64_t>(trial_hi, b.pc + b.ir.count * 4ull);
        }
        const auto compile_start = std::chrono::steady_clock::now();
        const size_t region_start = code_buf_used_;
        tier2_current_monitor_ = nullptr;
        const size_t old_edge_count = mt_edges_.size();
        const auto old_source_edges = mt_source_refs_[pc];
        uint64_t (*chain_fn)(CPU*, Emulator*) = nullptr;
        uint64_t (*rfn)(CPU*, Emulator*) =
            trace.ok ? compile_tier2_region(emu, trace, &chain_fn) : nullptr;
        tier2_compile_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - compile_start).count(), std::memory_order_relaxed);
        tier2_code_bytes.fetch_add(code_buf_used_ - region_start, std::memory_order_relaxed);
        const size_t region_bytes = code_buf_used_ - region_start;
        if (!rfn || !ensure_code_space(256)) {
            if (tier2_current_monitor_)
                tier2_current_monitor_->enabled.store(0, std::memory_order_release);
            // Unpublished region edges must not supersede the baseline's
            // source metadata. Their data cells stay alive but are revoked.
            for (size_t k = old_edge_count; k < mt_edges_.size(); ++k) {
                mt_edges_[k]->live = false;
                mt_edges_[k]->target.store(0, std::memory_order_release);
            }
            mt_source_refs_[pc] = old_source_edges;
            return;
        }
        if (rfn) {
            tier2_regions.fetch_add(1, std::memory_order_relaxed);
            if (tier2_trace_enabled()) {
                fprintf(stderr,
                        "[tier2] in-code region fired pc=0x%llx blocks=%zu insts=%llu\n",
                        static_cast<unsigned long long>(pc), trace.blocks.size(),
                        static_cast<unsigned long long>(trace.total_insts));
            }
            BlockEntry region_entry;
            region_entry.fn = rfn;
            region_entry.emitted_bytes = region_bytes;
            // Chain-skip: publish the post-frame-setup entry so chain edges
            // (bare jmps with the ROOT's rbp/rsp) skip the frame allocation.
            if (chain_skip_enabled() && chain_fn)
                region_entry.chain_entry = chain_fn;
            region_entry.instr_count = static_cast<int>(trace.total_insts);
            region_entry.tier2_hot_logged = true;
            region_entry.is_region = true;
            region_entry.tier2_monitor = tier2_current_monitor_;
            region_entry.ends_with_branch = true;
            region_entry.chained = true;        // region has no chain slot
            region_entry.taken_chained = true;
            region_entry.interp_only = false;
            region_entry.verified_once = true;  // M1: no region verify
            for (const auto& block : trace.blocks)
                region_entry.inlined_ranges_.push_back({block.pc, block.ir.count});
            for (const auto& block : trace.blocks) {
                auto baseline = blocks_.find(block.pc);
                if (baseline != blocks_.end()) {
                    tier2_counter_disable(baseline->second.tier2_counter_off, baseline->second.tier2_counter_len);
                    baseline->second.tier2_hot_logged = true;
                }
                const uint64_t end = block.pc + block.ir.count * 4;
                for (const auto& profile : tier2_profiles_by_pc_)
                    if (profile.first >= block.pc && profile.first < end)
                        profile.second->enabled.store(0, std::memory_order_release);
            }
            const auto wrapper_start_clock = std::chrono::steady_clock::now();
            auto trial = std::make_unique<Tier2Trial>();
            trial->monitor = tier2_current_monitor_;
            trial->baseline = old_entry;
            trial->candidate_entry = region_entry;
            trial->baseline.tier2_hot_logged = true;
            trial->candidate = rfn; trial->candidate_chain = chain_fn;
            trial->lo = trial_lo; trial->hi = trial_hi;
            trial->baseline_blocks = std::move(baseline_graph);
            trial->baseline_edges = old_source_edges;
            for (size_t k = old_edge_count; k < mt_edges_.size(); ++k)
                trial->candidate_edges.push_back(mt_edges_[k].get());
            const size_t sampling_start = code_buf_used_;
            const size_t sampling_edge_start = mt_edges_.size();
            struct SamplingCompileGuard {
                bool& flag;
                explicit SamplingCompileGuard(bool& f) : flag(f) { flag = true; }
                ~SamplingCompileGuard() { flag = false; }
            };
            bool samples_ok = true;
            {
                SamplingCompileGuard sampling(tier2_sampling_compile_);
                trial->baseline_blocks.clear();
                for (const auto& block : trace.blocks) {
                    BlockEntry copy;
                    if (!translate_block(emu, block.pc, &copy) || copy.instr_count != block.ir.count ||
                        copy.has_call || copy.has_svc || copy.has_selfloop_slot) {
                        samples_ok = false;
                        break;
                    }
                    trial->baseline_blocks.push_back({block.pc, copy.fn});
                    trial->sampling_blocks.push_back({block.pc, std::move(copy)});
                }
                if (samples_ok) {
                    trial->baseline_sample = trial->sampling_blocks.front().second.fn;
                    trial->baseline_chain_sample = trial->sampling_blocks.front().second.chain_entry;
                    trial->candidate_sample = compile_tier2_region(emu, trace,
                        &trial->candidate_chain_sample, trial->monitor);
                    samples_ok = trial->candidate_sample != nullptr;
                }
            }
            for (size_t k = sampling_edge_start; k < mt_edges_.size(); ++k)
                trial->sampling_edges.push_back(mt_edges_[k].get());
            tier2_code_bytes.fetch_add(code_buf_used_ - sampling_start, std::memory_order_relaxed);
            if (!samples_ok || !ensure_code_space(256)) {
                trial->monitor->enabled.store(0, std::memory_order_release);
                for (size_t k = old_edge_count; k < mt_edges_.size(); ++k) {
                    mt_edges_[k]->live = false;
                    mt_edges_[k]->target.store(0, std::memory_order_release);
                }
                mt_source_refs_[pc] = old_source_edges;
                return;
            }
            auto* trial_ptr = trial.get();
            tier2_current_monitor_->trial = trial_ptr;
            tier2_trials_.push_back(std::move(trial));
            // A full-frame entry selects live-work A/B runs. Backedges in an
            // active run stay entirely in native code until the quota expires.
            make_writable();
            const size_t wrapper = code_buf_used_;
            emit_mov_imm64(RDX, reinterpret_cast<uint64_t>(trial_ptr));
            emit_byte(0x48); emit_byte(0x39); emit_byte(0x97);
            emit_u32(offsetof(CPU, jit_tier2_trial)); // cmp [rdi+trial],rdx
            const size_t outside_trial = emit_jcc_rel32_placeholder(5);
            emit_byte(0x83); emit_byte(0xAF);
            emit_u32(offsetof(CPU, jit_tier2_trial_remaining)); emit_byte(1);
            const size_t quota = emit_jcc_rel32_placeholder(4);
            emit_load(R11, RDI, offsetof(CPU, jit_tier2_trial_fn));
            emit_byte(0x41); emit_byte(0xFF); emit_byte(0xE3);
            const size_t done = code_buf_used_;
            emit_load(RAX, RDI, PC_OFF); emit_ret();
            patch_jcc_rel32(quota, static_cast<int32_t>(done - (quota + 6)));
            patch_jcc_rel32(outside_trial, static_cast<int32_t>(code_buf_used_ - (outside_trial + 6)));
            emit_byte(0x48); emit_byte(0x83); emit_byte(0xBF);
            emit_u32(offsetof(CPU, jit_tier2_trial)); emit_byte(0);
            const size_t nested = emit_jcc_rel32_placeholder(5);
            patch_jcc_rel32(nested, static_cast<int32_t>(done - (nested + 6)));
            emit_mov_imm64(R11, reinterpret_cast<uint64_t>(&measured_region_entry));
            emit_byte(0x41); emit_byte(0xFF); emit_byte(0xE3);
            region_entry.fn = reinterpret_cast<uint64_t (*)(CPU*, Emulator*)>(code_buf_ + wrapper);
            if (chain_skip_enabled()) {
                const size_t chain_wrapper = code_buf_used_;
                emit_mov_imm64(RDX, reinterpret_cast<uint64_t>(trial_ptr));
                emit_byte(0x48); emit_byte(0x39); emit_byte(0x97);
                emit_u32(offsetof(CPU, jit_tier2_trial));
                const size_t cold_chain = emit_jcc_rel32_placeholder(5);
                emit_byte(0x83); emit_byte(0xAF);
                emit_u32(offsetof(CPU, jit_tier2_trial_remaining)); emit_byte(1);
                const size_t chain_quota = emit_jcc_rel32_placeholder(4);
                emit_load(R11, RDI, offsetof(CPU, jit_tier2_trial_chain_fn));
                emit_byte(0x41); emit_byte(0xFF); emit_byte(0xE3);
                const size_t cold_chain_tail = code_buf_used_;
                emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);
                emit_pop(R15); emit_pop(R14); emit_pop(R13);
                emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
                const size_t jump = emit_jmp_rel32_placeholder();
                patch_jmp_rel32(jump, static_cast<int32_t>(wrapper - (jump + 5)));
                // Quota also needs the root-frame unwind; emit a separate tail.
                const size_t quota_tail = code_buf_used_;
                emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC);
                emit_pop(R15); emit_pop(R14); emit_pop(R13);
                emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
                emit_load(RAX, RDI, PC_OFF); emit_ret();
                patch_jcc_rel32(cold_chain, static_cast<int32_t>(cold_chain_tail - (cold_chain + 6)));
                patch_jcc_rel32(chain_quota, static_cast<int32_t>(quota_tail - (chain_quota + 6)));
                region_entry.chain_entry = reinterpret_cast<uint64_t (*)(CPU*, Emulator*)>(code_buf_ + chain_wrapper);
            }
            // Bind private chains only to this sampling graph. Cold targets
            // stay unbound and return before executing unrelated guest work.
            auto sample_target = [&](uint64_t target, bool full) -> uint64_t (*)(CPU*, Emulator*) {
                if (target == pc)
                    return !full && region_entry.chain_entry ? region_entry.chain_entry : region_entry.fn;
                for (const auto& copy : trial_ptr->sampling_blocks)
                    if (copy.first == target)
                        return !full && copy.second.chain_entry ? copy.second.chain_entry : copy.second.fn;
                return nullptr;
            };
            for (auto* edge : trial_ptr->sampling_edges)
                edge->target.store(reinterpret_cast<uint64_t>(sample_target(edge->target_pc, edge->cold_entry)),
                    std::memory_order_release);
            if (!mt_active() && !dbg().no_chain) {
                for (const auto& copy : trial_ptr->sampling_blocks) {
                    for (auto exit : {std::pair<uint64_t, size_t>{copy.second.chain_target_pc, copy.second.chain_patch_off},
                                      {copy.second.taken_chain_target_pc, copy.second.taken_chain_patch_off}}) {
                        auto target = sample_target(exit.first, false);
                        if (!target || !exit.second) continue;
                        make_writable();
                        patch_chain(exit.second, reinterpret_cast<const uint8_t*>(target));
                    }
                }
            }
            make_executable();
            const size_t wrapper_bytes = code_buf_used_ - wrapper;
            tier2_code_bytes.fetch_add(wrapper_bytes, std::memory_order_relaxed);
            tier2_compile_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - wrapper_start_clock).count(), std::memory_order_relaxed);
            region_entry.emitted_bytes += wrapper_bytes;
            it->second = std::move(region_entry);
            for (size_t k = old_edge_count; k < mt_edges_.size(); ++k)
                refresh_mt_edges(mt_edges_[k]->target_pc);
            if (mt_active()) mt_regions_published_.fetch_add(1, std::memory_order_relaxed);
            redirect_tier2_head_locked(pc);

        }
    } catch (...) {
        // Never let a collect/compile/registration failure escape a JIT frame.
        // The RAII compiler lock is released on every failure.
    }
}

}  // namespace arm64emu
