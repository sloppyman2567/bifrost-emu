#include "audio/audio.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "audio counter check failed at %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    arm64emu::Audio audio;
    int stereo = audio.stream_open(arm64emu::PCM_FMT_F32, 48000, 2);
    int mono = audio.stream_open(arm64emu::PCM_FMT_S16, 24000, 1);
    CHECK(stereo && mono);
    audio.stream_pause(stereo, true);
    audio.stream_pause(mono, true);
    std::vector<uint8_t> floats(4800 * 8), shorts(2400 * 2);
    CHECK(audio.stream_write(stereo, arm64emu::PCM_FMT_F32, 48000, 2, floats.data(), floats.size()) == 4800);
    CHECK(audio.stream_write(mono, arm64emu::PCM_FMT_S16, 24000, 1, shorts.data(), shorts.size()) == 2400);
    uint64_t written, read;
    CHECK(audio.stream_frame_counters(stereo, written, read) && written == 4800 && read == 0);
    CHECK(audio.stream_frame_counters(mono, written, read) && written == 2400 && read == 0);
    audio.stream_pause(stereo, false);
    audio.stream_pause(mono, false);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((audio.stream_queued_frames(stereo) || audio.stream_queued_frames(mono)) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(audio.stream_frame_counters(stereo, written, read) && written == 4800 && read == 4800);
    CHECK(audio.stream_frame_counters(mono, written, read) && written == 2400 && read == 2400);
    audio.stream_pause(mono, true);
    CHECK(audio.stream_write(mono, arm64emu::PCM_FMT_S16, 24000, 1, shorts.data(), shorts.size()) == 2400);
    audio.stream_clear(mono);
    CHECK(audio.stream_frame_counters(mono, written, read) && written == 4800 && read == 2400);
    audio.stream_pause(stereo, true);
    floats.resize(48000 * 8);
    auto accepted = audio.stream_write(stereo, arm64emu::PCM_FMT_F32, 48000, 2, floats.data(), floats.size());
    CHECK(accepted > 0 && accepted < 48000);
    CHECK(audio.stream_frame_counters(stereo, written, read) && written == uint64_t(4800 + accepted) && read == 4800);
    CHECK(audio.stream_write(stereo, arm64emu::PCM_FMT_F32, 48000, 2, floats.data(), floats.size()) == 0);
    CHECK(audio.stream_frame_counters(stereo, written, read) && written == uint64_t(4800 + accepted));
    audio.stream_close(mono);
    CHECK(!audio.stream_frame_counters(mono, written, read));
    CHECK(audio.stream_frame_counters(stereo, written, read));
    audio.stream_close(stereo);
    std::puts("audio_frame_counters: checks passed");
}
