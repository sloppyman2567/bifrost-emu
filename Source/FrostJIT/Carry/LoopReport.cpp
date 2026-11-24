// LoopReport.cpp — Carry milestone 2: which hot code is a loop of several blocks, and how many guest registers it touches.
//
// Report only. With BIFROST_PROF and BIFROST_PC_HIST, translate_block records for each block the guest registers it
// reads before writing and the ones it writes (note_carry_block). At the profile report, dump_carry_candidates joins the
// sampled hotness of the hottest blocks with each block's static successors (the chain targets) and prints the cycles
// among them: the hot loops that a register-resident second JIT could keep in host registers. No code is emitted or changed.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "FrostJIT/Runtime/SampleProfile.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace arm64emu {

void FrostJIT::note_carry_block(uint64_t pc, const IRBlock& block) {
    uint32_t reads = 0, writes = 0;
    for (const IRInst& inst : block.insts) {
        if (inst.op == IROp::LOAD_REG && !inst.is_fp_load() && inst.src1 <= 31) {
            if (!((writes >> inst.src1) & 1)) reads |= 1u << inst.src1;
        } else if (inst.op == IROp::STORE_REG && !inst.is_fp_store() && inst.dest <= 31) {
            writes |= 1u << inst.dest;
        }
    }
    carry_info_[pc] = {reads, writes};
}

namespace {

struct HotNode {
    uint64_t pc = 0;
    uint64_t samples = 0;
    uint32_t reads = 0, writes = 0;
    int instrs = 0;
    std::vector<int> succ;  // indexes of hot blocks reached by a static edge
    bool self_edge = false;
    int targets = 0;  // static successors, hot or not
    // Tarjan
    int index = -1, low = 0;
    bool on_stack = false;
    int component = -1;
};

struct Tarjan {
    std::vector<HotNode>& nodes;
    std::vector<int> stack;
    int next_index = 0;
    std::vector<std::vector<int>> components;
    explicit Tarjan(std::vector<HotNode>& n) : nodes(n) {}
    void visit(int v) {
        nodes[v].index = nodes[v].low = next_index++;
        stack.push_back(v);
        nodes[v].on_stack = true;
        for (int w : nodes[v].succ) {
            if (nodes[w].index < 0) {
                visit(w);
                nodes[v].low = std::min(nodes[v].low, nodes[w].low);
            } else if (nodes[w].on_stack) {
                nodes[v].low = std::min(nodes[v].low, nodes[w].index);
            }
        }
        if (nodes[v].low != nodes[v].index) return;
        std::vector<int> members;
        int w;
        do {
            w = stack.back();
            stack.pop_back();
            nodes[w].on_stack = false;
            nodes[w].component = static_cast<int>(components.size());
            members.push_back(w);
        } while (w != v);
        components.push_back(std::move(members));
    }
};

int popcount32(uint32_t v) { return __builtin_popcount(v); }

}  // namespace

void FrostJIT::dump_carry_candidates(const ProfReport& report) {
    // Same locks as dump_memory_stats: compilers take compile_mutex_ before blocks_mutex_, and carry_info_ is written under
    // them. At exit a stopped guest thread may still own the compiler: skip rather than wait.
    std::unique_lock<std::mutex> compiling(compile_mutex_, std::defer_lock);
    for (int attempt = 0; attempt < 100 && !compiling.try_lock(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!compiling.owns_lock()) {
        std::fprintf(stderr, "[carry] skipped, compiler busy\n");
        return;
    }
    std::shared_lock<std::shared_mutex> lock(blocks_mutex_);
    const uint64_t jit_samples = report.total.entry + report.total.body + report.total.exit + report.total.cold;
    if (!jit_samples || report.hottest.empty()) return;

    std::vector<HotNode> nodes;
    std::unordered_map<uint64_t, int> index;
    for (const ProfBlockReport& hot : report.hottest) {
        if (index.count(hot.guest_pc)) continue;
        auto entry = blocks_.find(hot.guest_pc);
        auto info = carry_info_.find(hot.guest_pc);
        if (entry == blocks_.end() || info == carry_info_.end()) continue;  // retired since it was sampled
        HotNode node;
        node.pc = hot.guest_pc;
        node.samples = hot.counts.entry + hot.counts.body + hot.counts.exit + hot.counts.cold;
        node.reads = info->second.reads;
        node.writes = info->second.writes;
        node.instrs = entry->second.instr_count;
        index[node.pc] = static_cast<int>(nodes.size());
        nodes.push_back(std::move(node));
    }
    for (HotNode& node : nodes) {
        const BlockEntry& entry = blocks_.at(node.pc);
        if (entry.has_selfloop_slot) node.self_edge = true;  // a self-loop's back edge is its own slot, not a chain target
        for (uint64_t target : {entry.chain_target_pc, entry.taken_chain_target_pc}) {
            if (!target) continue;
            ++node.targets;
            if (target == node.pc) node.self_edge = true;
            auto it = index.find(target);
            if (it != index.end()) node.succ.push_back(it->second);
        }
    }
    Tarjan tarjan(nodes);
    for (int v = 0; v < static_cast<int>(nodes.size()); ++v)
        if (nodes[v].index < 0) tarjan.visit(v);

    struct Loop {
        std::vector<int> members;
        uint64_t samples = 0;
        int instrs = 0, moves = 0, registers = 0, reads = 0, writes = 0, exits = 0;
        uint64_t head_pc = 0;
    };
    std::vector<Loop> loops;
    uint64_t covered = 0, in_multi = 0, in_single = 0;
    for (const HotNode& node : nodes) covered += node.samples;
    for (const std::vector<int>& members : tarjan.components) {
        const bool single = members.size() == 1;
        if (single && !nodes[members[0]].self_edge) continue;
        Loop loop;
        loop.members = members;
        uint32_t reads = 0, writes = 0;
        int hottest = members[0];
        for (int m : members) {
            const HotNode& node = nodes[m];
            loop.samples += node.samples;
            loop.instrs += node.instrs;
            loop.moves += popcount32(node.reads) + popcount32(node.writes);
            reads |= node.reads;
            writes |= node.writes;
            if (node.samples > nodes[hottest].samples) hottest = m;
            int inside = 0;
            for (int s : node.succ)
                if (nodes[s].component == nodes[m].component) ++inside;
            loop.exits += node.targets - inside;
        }
        loop.head_pc = nodes[hottest].pc;
        loop.registers = popcount32(reads | writes);
        loop.reads = popcount32(reads);
        loop.writes = popcount32(writes);
        (single ? in_single : in_multi) += loop.samples;
        if (!single) loops.push_back(std::move(loop));
    }
    std::sort(loops.begin(), loops.end(), [](const Loop& a, const Loop& b) { return a.samples > b.samples; });

    auto pct = [&](uint64_t value) { return 100.0 * static_cast<double>(value) / static_cast<double>(jit_samples); };
    std::fprintf(stderr, "[carry] %zu hot blocks cover %.1f%% of the JIT samples: in loops of several blocks %.1f%%, "
                         "in single-block loops (already pinned) %.1f%%, elsewhere %.1f%%\n",
                 nodes.size(), pct(covered), pct(in_multi), pct(in_single), pct(covered - in_multi - in_single));
    size_t shown = 0;
    for (const Loop& loop : loops) {
        if (shown++ == 10) break;
        std::fprintf(stderr, "[carry] loop head=0x%llx blocks=%zu instrs=%d samples=%.1f%% registers=%d (read %d, written %d) "
                             "moves per pass=%d static exits=%d%s\n",
                     static_cast<unsigned long long>(loop.head_pc), loop.members.size(), loop.instrs, pct(loop.samples),
                     loop.registers, loop.reads, loop.writes, loop.moves, loop.exits, loop.registers <= 8 ? "" : " (over 8)");
    }
}

}  // namespace arm64emu
