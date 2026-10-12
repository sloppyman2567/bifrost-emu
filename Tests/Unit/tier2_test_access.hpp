#pragma once
// Codegen/retirement oracles use deterministic candidate selection. Production
// timing decisions are separately exercised with real controlled slowdowns.
namespace arm64emu {
struct Tier2TestAccess {
    static bool vector_cache_active(const FrostJIT& jit) {
        return jit.vec_cache_active_;
    }
    static bool supports_vector_cache(const FrostJIT& jit) { return jit.has_fma3(); }
    static bool region_invariants_preserved(FrostJIT& jit,const Tier2Trace& trace) {
        Tier2RegionPlan plan;
        if (!jit.plan_tier2_region(trace,nullptr,plan)) return false;
        if (!plan.last_is_backedge) return true;
        bool invariant[4096]={};
        for (const auto& inst : plan.preheader)
            if (inst.dest>32 && inst.dest<4096) invariant[inst.dest]=true;
        for (const auto& inst : plan.region_ir)
            if (inst.dest>32 && inst.dest<4096 && invariant[inst.dest]) return false;
        return true;
    }
    static void keep_candidates(FrostJIT& jit) {
        for (auto& trial : jit.tier2_trials_) {
            auto head = jit.blocks_.find(trial->monitor->head);
            if (head == jit.blocks_.end() || head->second.tier2_monitor != trial->monitor ||
                !trial->monitor->enabled.load()) continue;
            trial->decision.store(1);
            jit.finish_tier2_trial(*trial, true);
        }
    }
};
}
