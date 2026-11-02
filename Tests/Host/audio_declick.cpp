// StreamDeclick: a stream dropping out of the mix and returning must not step the output.
#include "System/Audio/StreamDeclick.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "audio declick check failed at %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
namespace {
constexpr size_t kChunk = 1024, kFade = 132;  // 3 ms at 44.1 kHz
constexpr float kPi = 3.14159265f;
// A 200 Hz wave at 0.8: its steepest honest step between samples.
const float kWaveStep = 0.8f * 2 * kPi * 200 / 44100;
float wave(size_t frame) { return 0.8f * std::sin(2 * kPi * 200 * float(frame) / 44100); }

struct Run {
    arm64emu::StreamDeclick declick;
    std::vector<float> output;
    size_t read = 0;
    // One device callback in which the stream supplies `available` frames.
    void callback(size_t available) {
        std::vector<float> mix(kChunk * 2, 0.0f);
        declick.mix(mix.data(), kChunk, available, 2, kFade, [&](size_t f, size_t) { return wave(read + f); });
        read += available;
        for (size_t f = 0; f < kChunk; ++f) output.push_back(mix[f * 2]);
    }
    float largest_step() const {
        float largest = std::fabs(output[0]);
        for (size_t i = 1; i < output.size(); ++i) largest = std::max(largest, std::fabs(output[i] - output[i - 1]));
        return largest;
    }
};
// A jump the fade cannot explain: the wave's own step, plus the fade's share of full level.
const float kLimit = kWaveStep + 0.8f / kFade + 0.001f;
}  // namespace

int main() {
    // Steady playback passes through untouched once it has risen.
    {
        Run run;
        for (int i = 0; i < 4; ++i) run.callback(kChunk);
        CHECK(run.largest_step() <= kLimit);
        for (size_t f = kFade; f < run.output.size(); ++f) CHECK(std::fabs(run.output[f] - wave(f)) < 1e-5f);
    }
    // Paused mid-wave, at the wave's peak, for one chunk (the stop and restart of a game's sound).
    {
        Run run;
        run.read = 55 - kChunk % 55;  // a chunk boundary near the peak of the 220.5-sample cycle
        run.callback(kChunk);
        CHECK(std::fabs(run.output.back()) > 0.3f);
        run.callback(0);
        run.callback(kChunk);
        CHECK(run.largest_step() <= kLimit);
    }
    // Paused for many chunks: the fade ends in true silence, and nothing is left behind.
    {
        Run run;
        run.callback(kChunk);
        for (int i = 0; i < 3; ++i) run.callback(0);
        for (size_t f = kChunk + kFade; f < run.output.size(); ++f) CHECK(run.output[f] == 0.0f);
        CHECK(run.largest_step() <= kLimit);
    }
    // Starved partway through a callback, then back: the partial chunk does not step either.
    {
        Run run;
        run.callback(kChunk);
        run.callback(300);
        run.callback(0);
        run.callback(kChunk);
        CHECK(run.largest_step() <= kLimit);
    }
    // Dropping out again while the previous fade still runs (rapid restarts) stays bounded.
    {
        Run run;
        run.callback(kChunk);
        run.callback(0);
        run.callback(40);
        run.callback(40);
        run.callback(kChunk);
        CHECK(run.largest_step() <= 2 * kLimit);
    }
    // Without the fade the same stop is a real jump, so these checks can fail.
    {
        std::vector<float> raw;
        size_t read = 55 - kChunk % 55;
        for (size_t f = 0; f < kChunk; ++f) raw.push_back(wave(read + f));
        for (size_t f = 0; f < kChunk; ++f) raw.push_back(0.0f);
        float largest = 0;
        for (size_t i = 1; i < raw.size(); ++i) largest = std::max(largest, std::fabs(raw[i] - raw[i - 1]));
        CHECK(largest > 5 * kLimit);
    }
    std::puts("audio_declick: checks passed");
}
