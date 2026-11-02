// StreamDeclick.hpp — keeps a stream from stepping the mix when it joins or leaves it.
//
// A stream can drop out anywhere in its wave: it is paused, cleared or starved. Its contribution
// to the mix then falls to zero in one sample, and comes back the same way, which is heard as a
// pop. Here the last value it contributed fades to silence instead, and a returning stream
// fades in. Queued audio is not consumed or reordered by this, so frame counters are unaffected.
#pragma once
#include <cstddef>
#include <vector>
namespace arm64emu {
class StreamDeclick {
public:
    // Adds one device callback of a stream to `out` (`frames` interleaved frames of `channels`).
    // The stream supplies its first `available` frames, through sample(frame, channel).
    template <typename Sample>
    void mix(float* out, size_t frames, size_t available, size_t channels, size_t fade, Sample sample) {
        if (tail_.size() != channels) {
            tail_.assign(channels, 0.0f);
            last_.assign(channels, 0.0f);
        }
        if (available && !sounding_) rise_ = 0;
        for (size_t f = 0; f < available; ++f) {
            const float gain = rise_ < fade ? float(rise_ + 1) / float(fade + 1) : 1.0f;
            if (rise_ < fade) ++rise_;
            for (size_t c = 0; c < channels; ++c) {
                const float value = sample(f, c) * gain;
                out[f * channels + c] += value;
                last_[c] = value;
            }
        }
        const bool leaving = available < frames && (sounding_ || available);
        size_t f = 0;
        // A fade still running continues under whatever the stream is doing now.
        render_tail(out, f, leaving ? available : frames, channels, fade);
        if (leaving) {
            for (size_t c = 0; c < channels; ++c)
                tail_[c] = tail_[c] * float(tail_left_) / float(fade + 1) + last_[c];
            tail_left_ = fade;
            f = available;
            render_tail(out, f, frames, channels, fade);
        }
        sounding_ = available == frames;
    }

private:
    void render_tail(float* out, size_t& f, size_t end, size_t channels, size_t fade) {
        for (; f < end && tail_left_; ++f, --tail_left_) {
            const float gain = float(tail_left_) / float(fade + 1);
            for (size_t c = 0; c < channels; ++c) out[f * channels + c] += tail_[c] * gain;
        }
        f = end;
    }
    std::vector<float> tail_, last_;
    size_t tail_left_ = 0;
    size_t rise_ = 0;
    bool sounding_ = false;
};
}  // namespace arm64emu
