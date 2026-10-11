#pragma once
// Codegen/retirement oracles use deterministic candidate selection. Production
// timing decisions are separately exercised with real controlled slowdowns.
namespace arm64emu {
struct Tier2TestAccess {
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
