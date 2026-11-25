// RegisterPassing.cpp — guest registers handed from block to block in host registers (BIFROST_CARRY).
//
// A chain edge used to hand over nothing: the predecessor stored its guest registers, and the successor loaded the ones it reads
// first from cpu.regs[]. With register passing the predecessor also leaves clean copies of guest registers in host registers at
// its chain slots (a layout), and a successor with a register entry, which is its normal entry minus the loads of the registers in
// its entry layout, is entered there when the layout covers every one of them. Memory stays authoritative: a copy is never ahead of
// cpu.regs[], so fault recovery, signals and the dispatcher entries see the state they always did.
//
// Copies are made where they cost nothing: a temporary that dies while it still holds a guest register's value (loaded from it, or
// stored to it) stays in its host register as that register's mapping, instead of being killed (rp_release), and a LOAD_REG of a
// register that is mapped clean takes the host register over instead of copying it. Both sides pick host registers so that layouts
// agree without negotiation: a block's entry layout follows the layouts its already translated predecessors leave, its results go
// where its already translated successors expect them, and a guest register's canonical host register (rp_host_for) is the default.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/DebugFlags.hpp"

namespace arm64emu {

// The ops the entry-set scan may look through: none of them calls out, writes a guest register behind the allocator, or ends the
// block. The scan stops at anything else, so a register read only after a call or a mid-block exit is not preloaded.
static bool rp_straight_line(IROp op) {
    switch (op) {
        case IROp::NOP: case IROp::IMM: case IROp::MOV:
        case IROp::LOAD_REG: case IROp::STORE_REG:
        case IROp::LOAD_MEM: case IROp::STORE_MEM:
        case IROp::ADD: case IROp::SUB: case IROp::MUL:
        case IROp::AND: case IROp::OR: case IROp::XOR:
        case IROp::SHL: case IROp::SHR: case IROp::SAR: case IROp::ROR:
        case IROp::NOT: case IROp::NEG: case IROp::SEXT: case IROp::ZEXT:
        case IROp::ADDS: case IROp::SUBS: case IROp::TST:
        case IROp::UBFM: case IROp::SBFM:
            return true;
        default:
            return false;
    }
}

// The host registers the translated predecessors of `pc` leave guest registers in: host_of[g] is the host register of the first one
// that holds g, or -1. Single-thread code finds its predecessors through back_refs_, multithreaded code through its edge cells.
// Returns how many predecessor slots were found.
int FrostJIT::rp_pred_hosts(uint64_t pc, int8_t host_of[31]) const {
    int found = 0;
    for (int g = 0; g < 31; g++) host_of[g] = -1;
    auto take = [&](uint64_t layout) {
        for (int i = 1; i <= static_cast<int>(layout & 0xFF); i++) {
            const uint8_t entry = static_cast<uint8_t>(layout >> (8 * i));
            if (host_of[entry >> 3] < 0) host_of[entry >> 3] = static_cast<int8_t>(RP_HOSTS[entry & 7]);
        }
    };
    if (mt_active()) {
        const auto refs = mt_edge_refs_.find(pc);
        if (refs == mt_edge_refs_.end()) return 0;
        for (const MtEdge* edge : refs->second) {
            if (!edge->live || edge->internal_loop) continue;
            const auto source = blocks_.find(edge->source_pc);
            if (source == blocks_.end()) continue;
            const BlockEntry& e = source->second;
            ++found;
            const RegPass* info = reg_pass_of(e);
            if (!info) continue;
            if (edge->slot == e.chain_patch_off) take(info->exit_layout);
            else if (e.has_taken_chain_slot && edge->slot == e.taken_chain_patch_off) take(info->taken_layout);
        }
        return found;
    }
    const auto refs = back_refs_.find(pc);
    if (refs == back_refs_.end()) return 0;
    for (uint64_t key : refs->second) {
        const auto source = blocks_.find(key);
        if (source == blocks_.end()) continue;
        const BlockEntry& e = source->second;
        ++found;
        const RegPass* info = reg_pass_of(e);
        if (!info) continue;
        if (e.chain_target_pc == pc) take(info->exit_layout);
        if (e.has_taken_chain_slot && e.taken_chain_target_pc == pc) take(info->taken_layout);
    }
    return found;
}

// The guest registers this block reads before it writes them, in the order it reads them, at most RP_MAX_ENTRIES: the ones a
// predecessor's copies can replace the loads of. When translated predecessors exist, only the registers they leave in host registers
// qualify (a register no predecessor holds would make every edge from them miss the entry, and loads lazily as it always did),
// each in the host register they leave it in; with none, the first ones read, in the host register the successors want. No two
// share a host register.
void FrostJIT::rp_select_entry(const IRBlock& block, uint64_t start_pc) {
    rp_entry_layout_ = 0;
    rp_entry_n_ = 0;
    int8_t pred_host[31];
    const bool have_preds = rp_pred_hosts(start_pc, pred_host) > 0;
    bool seen[32] = {};
    unsigned used_hosts = 0;
    for (size_t i = 0; i < block.insts.size() && rp_entry_n_ < RP_MAX_ENTRIES; i++) {
        const IRInst& inst = block.insts[i];
        if (!rp_straight_line(inst.op)) break;
        if (inst.op == IROp::LOAD_REG) {
            if (inst.is_fp_load() || inst.src1 > 30 || seen[inst.src1]) continue;
            seen[inst.src1] = true;
            if (have_preds && pred_host[inst.src1] < 0) continue;
            int host = pred_host[inst.src1] >= 0 ? pred_host[inst.src1] : rp_want_host_[inst.src1];
            if (used_hosts & (1u << host)) continue;
            used_hosts |= 1u << host;
            rp_entry_guest_[rp_entry_n_] = inst.src1;
            rp_entry_host_[rp_entry_n_] = host;
            ++rp_entry_n_;
            rp_entry_layout_ = rp_layout_add(rp_entry_layout_, inst.src1, host);
        } else if (inst.op == IROp::STORE_REG) {
            if (!inst.is_fp_store() && inst.dest < 32) seen[inst.dest] = true;
            if (inst.src1 < 32) seen[inst.src1] = true;
        } else if (writes_arch_gpr_directly(inst)) {
            seen[inst.dest] = true;
        }
    }
}

void FrostJIT::rp_clear_hints() {
    for (uint16_t temp : rp_pref_set_) rp_pref_[temp] = 0;
    rp_pref_set_.clear();
    rp_dest_hint_ = -1;
}

// Where this block's results should go: a guest register's value is best left where a translated successor of the block expects it
// (its entry layout), else in its canonical host register. A temporary is defined in the host register of the guest register it is
// finally stored to; the last store wins.
void FrostJIT::rp_prepare_hints(const IRBlock& block, uint64_t start_pc) {
    rp_clear_hints();
    for (int g = 0; g < 31; g++) rp_want_host_[g] = static_cast<int8_t>(rp_host_for(g));
    // The static successors: both ends of a conditional branch, the target of a direct one, else the next block in memory.
    uint64_t targets[2];
    int target_count = 0;
    if (!block.insts.empty()) {
        const IRInst& last = block.insts.back();
        switch (last.op) {
            case IROp::BRCOND_FALLTHRU: targets[target_count++] = last.fallthru_params().target; break;
            case IROp::BRCOND: case IROp::BRCOND_ZERO: case IROp::BRCOND_BIT:
                targets[target_count++] = last.arm_pc + 4;
                targets[target_count++] = last.branch_target();
                break;
            default: targets[target_count++] = start_pc + static_cast<uint64_t>(block.count) * 4; break;
        }
    }
    for (int t = 0; t < target_count; t++) {
        const auto known = blocks_.find(targets[t]);
        if (known == blocks_.end()) continue;
        const RegPass* info = reg_pass_of(known->second);
        if (!info || !info->regs_entry_off) continue;
        for (int i = 1; i <= static_cast<int>(info->entry_layout & 0xFF); i++) {
            const uint8_t entry = static_cast<uint8_t>(info->entry_layout >> (8 * i));
            rp_want_host_[entry >> 3] = static_cast<int8_t>(RP_HOSTS[entry & 7]);
        }
    }
    for (const IRInst& inst : block.insts) {
        if (inst.op != IROp::STORE_REG || inst.is_fp_store() || inst.dest > 30 || inst.src1 <= 32 || inst.src1 >= 4096) continue;
        if (!rp_pref_[inst.src1]) rp_pref_set_.push_back(inst.src1);
        rp_pref_[inst.src1] = static_cast<uint8_t>(rp_want_host_[inst.dest] + 1);
    }
}

// The wanted register of the op being compiled's result, if it can have it: free, or holding a clean copy of a guest register that
// nothing reads again.
int FrostJIT::rp_take_hint(int excl1, int excl2) {
    const int host = rp_dest_hint_;
    if (host < 0 || host == excl1 || host == excl2 || (pinned_host_regs_ & (1u << host))) return -1;
    const int holder = reg_vreg_[host];
    if (holder >= 0) {
        if (holder > 30 || vreg_dirty_[holder] || next_use_after(holder, cur_op_index_) >= 0) return -1;
        kill_vreg(holder);
    }
    return host;
}

// The loads of the entry set, then the mapping the body starts with: each register clean in its host register.
void FrostJIT::rp_emit_entry_loads() {
    for (int i = 0; i < rp_entry_n_; i++) {
        const int guest = rp_entry_guest_[i], host = rp_entry_host_[i];
        emit_load_arm(host, guest);
        vreg_home_[guest] = host;
        reg_vreg_[host] = guest;
        vreg_dirty_[guest] = false;
        vreg_last_use_[guest] = ++regalloc_lru_counter_;
        if (guest > max_vreg_) max_vreg_ = guest;
    }
}

void FrostJIT::rp_unbind_temp(int temp) {
    if (temp < 0 || temp >= 4096 || !rp_arch_of_[temp]) return;
    const int guest = rp_arch_of_[temp] - 1;
    if (rp_binding_[guest] == temp) rp_binding_[guest] = 0;
    rp_arch_of_[temp] = 0;
}

void FrostJIT::rp_unbind(int guest) {
    if (!rp_mirrors_ || guest < 0 || guest > 30 || rp_binding_[guest] <= 0) return;
    rp_arch_of_[rp_binding_[guest]] = 0;
    rp_binding_[guest] = 0;
}

void FrostJIT::rp_unbind_all() {
    if (rp_bound_temps_.empty()) return;
    for (int guest = 0; guest <= 30; guest++) rp_binding_[guest] = 0;
    for (uint16_t temp : rp_bound_temps_) rp_arch_of_[temp] = 0;
    rp_bound_temps_.clear();
}

// `temp` now holds guest register `guest`'s value, and memory holds it too (it was loaded from the register, or just stored to it).
void FrostJIT::rp_bind(int guest, int temp) {
    if (!rp_mirrors_ || guest < 0 || guest > 30 || temp <= 32 || temp >= 4096) return;
    rp_unbind(guest);
    rp_unbind_temp(temp);
    rp_binding_[guest] = static_cast<int16_t>(temp);
    rp_arch_of_[temp] = static_cast<uint8_t>(guest + 1);
    rp_bound_temps_.push_back(static_cast<uint16_t>(temp));
}

// A temporary's last use has passed. If it still holds a guest register's value in a host register a chain edge can pass, the
// register's own mapping takes the host register over (clean: memory has the value) instead of the temporary dying.
void FrostJIT::rp_release(int v) {
    if (rp_mirrors_ && v > 32 && v < 4096 && rp_arch_of_[v]) {
        const int guest = rp_arch_of_[v] - 1;
        const int host = vreg_home_[v];
        if (rp_binding_[guest] == v && host >= 0 && reg_vreg_[host] == v && rp_host_index(host) >= 0 && vreg_home_[guest] < 0) {
            vreg_home_[v] = -1;
            vreg_dirty_[v] = false;
            vreg_last_use_[v] = 0;
            dirty_host_regs_ &= ~(1u << host);
            reg_vreg_[host] = guest;
            vreg_home_[guest] = host;
            vreg_dirty_[guest] = false;
            vreg_last_use_[guest] = ++regalloc_lru_counter_;
            if (guest > max_vreg_) max_vreg_ = guest;
            rp_unbind_temp(v);
            return;
        }
        rp_unbind_temp(v);
    }
    kill_vreg(v);
}

// The clean copies of guest registers in host registers a chain slot emitted now would leave to its target. RAX (the next pc) and
// R11 (the MT cell jump) are not in RP_HOSTS; nothing else runs between the capture and the slot.
uint64_t FrostJIT::rp_capture_layout() const {
    if (!rp_mirrors_) return 0;
    uint64_t layout = 0;
    for (int guest = 0; guest <= 30; guest++) {
        const int host = vreg_home_[guest];
        if (host < 0 || vreg_dirty_[guest] || reg_vreg_[host] != guest || rp_host_index(host) < 0) continue;
        layout = rp_layout_add(layout, guest, host);
    }
    return layout;
}

// The entry of `target` a chain slot of `source` at code offset `slot` should jump to: its register entry when the layout the slot
// leaves covers the target's entry layout, its plain chain entry otherwise.
const uint8_t* FrostJIT::chain_target_for(const BlockEntry& source, size_t slot, const BlockEntry& target) const {
    const uint8_t* plain = (chain_skip_enabled() && target.chain_entry)
        ? reinterpret_cast<const uint8_t*>(target.chain_entry) : reinterpret_cast<const uint8_t*>(target.fn);
    const RegPass* want = reg_pass_of(target);
    const RegPass* have = reg_pass_of(source);
    if (!want || !want->regs_entry_off || !want->entry_layout || !chain_skip_enabled()) return plain;
    uint64_t layout = 0;
    if (have && slot == source.chain_patch_off) layout = have->exit_layout;
    else if (have && source.has_taken_chain_slot && slot == source.taken_chain_patch_off) layout = have->taken_layout;
    if (layout && rp_layout_covers(layout, want->entry_layout)) {
        ++rp_edges_passed_;
        return code_buf_ + want->regs_entry_off;
    }
    ++rp_edges_plain_;
    return plain;
}

}  // namespace arm64emu
