// AudioThunk::dispatch — resolves the symbol and runs the per-API stages.
#include "System/Audio/AudioThunk/AudioThunkInternal.hpp"
namespace arm64emu {
int64_t AudioThunk::dispatch(CPU& cpu, uint32_t symbol_id) {
    if (!impl_ || !impl_->enabled || !impl_->initialized) return -ENOSYS;
    if ((symbol_id & AudioThunk::ID_MASK) != AudioThunk::ID_BASE) return -ENOENT;
    uint32_t local_id = symbol_id - AudioThunk::ID_BASE;
    if (local_id >= impl_->id_to_idx_.size()) return -ENOENT;
    auto [lib_idx, ent_idx] = impl_->id_to_idx_[local_id];
    const auto& entry = impl_->libs_[lib_idx].entries[ent_idx];
    const std::string name = entry.name;
    AudioThunkImpl& I = *impl_;
    const bool trace = dbg().thunk_trace;
    AudioDispatch d{*this, I, cpu, entry, name, trace};
    for (auto stage : {&AudioDispatch::sdl, &AudioDispatch::alsa, &AudioDispatch::pulse,
                       &AudioDispatch::openal, &AudioDispatch::aaudio, &AudioDispatch::opensl}) {
        if (auto handled = (d.*stage)()) return *handled;
    }
    // ══ fallback: generic host-fn dispatch (legacy rows) ════════════
    return thunk_dispatch_generic(cpu, entry.host_fn, entry.name, trace);
}
}  // namespace arm64emu
