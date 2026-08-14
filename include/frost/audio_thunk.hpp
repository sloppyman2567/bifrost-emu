// frost/audio_thunk.hpp — AudioThunk: forward guest audio calls to host.
//
// 1.5.2-alpha: NEW. AudioThunk is the audio counterpart to GraphicThunk.
// It intercepts guest dlsym calls for libasound (ALSA), libpulse (PulseAudio),
// libSDL2 (audio subsystem), and libopenal, and forwards them to the host's
// equivalent libraries.
//
// Like GraphicThunk, AudioThunk:
//   - Allocates a guest trampoline page (16 bytes per registered symbol).
//   - Each trampoline traps via __NR_bifrost_thunk (case 0x1000).
//   - The host's dispatch() reads args from x0..x7, calls the host function,
//     writes the return value to x0.
//
// Supported libraries (partial):
//   - libasound.so.2 (ALSA): snd_pcm_open, snd_pcm_close, snd_pcm_hw_params_*,
//     snd_pcm_writei, snd_pcm_readi, snd_pcm_drain, snd_pcm_drop, snd_pcm_pause.
//   - libpulse.so.0 (PulseAudio): pa_simple_new, pa_simple_write,
//     pa_simple_drain, pa_simple_free.
//   - libSDL2.so (audio subset): SDL_OpenAudioDevice, SDL_CloseAudioDevice,
//     SDL_PauseAudioDevice, SDL_QueueAudio, SDL_GetQueuedAudioSize,
//     SDL_DequeueAudio, SDL_AudioInit, SDL_AudioQuit.
//   - libopenal.so.1 (OpenAL): alcOpenDevice, alcCloseDevice, alcCreateContext,
//     alcMakeContextCurrent, alcDestroyContext, alGenSources, alDeleteSources,
//     alSourcePlay, alSourceStop, alSourceQueueBuffers, alSourceUnqueueBuffers,
//     alBufferData, alGenBuffers, alDeleteBuffers.
//
// Pointer-arg translation: the thunk does NOT translate pointer args.
// Audio APIs use guest-allocated structs (snd_pcm_hw_params_t is opaque
// and dynamically sized; pa_sample_spec is a small struct). The guest
// would need to allocate these in shared memory, which is a non-trivial
// change to the dynamic linker. For now, we thunk only the entry points
// that take simple integer/pointer args (handle, format enum, count) and
// stub out the rest. Real audio playback still goes through /dev/dsp
// (the Audio class) — AudioThunk is for guests that explicitly use ALSA/
// PulseAudio/SDL2/OpenAL APIs.
//
// The thunk is opt-in: set BIFROST_THUNK_AUDIO=1 (or [thunk] audio = true
// in the config file). Without it, resolve() returns 0 and dispatch() is
// a no-op.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace arm64emu {
class Memory;
class CPU;
// Forward-declare the pimpl.
struct AudioThunkImpl;
class AudioThunk {
public:
    AudioThunk();
    ~AudioThunk();
    AudioThunk(const AudioThunk&) = delete;
    AudioThunk& operator=(const AudioThunk&) = delete;
    bool enabled() const;
    // Lifecycle: allocate trampoline page, register known symbols.
    // Idempotent. Returns true on success.
    bool init(Memory& mem);
    // Resolve a (library, symbol) pair to a guest-callable trampoline
    // address, or 0 if not thunked. Used by the dynamic linker.
    uint64_t resolve(const std::string& lib, const std::string& sym);
    // Enumerate symbols for `lib`. Returns count.
    size_t enumerate_symbols(const std::string& lib,
        const std::function<void(const std::string&, uint64_t)>& cb) const;
    // Dispatch a thunk call. Returns 0 on success, -errno on failure.
    int64_t dispatch(CPU& cpu, uint32_t symbol_id);
    // Diagnostics.
    size_t symbol_count() const;
    uint64_t trampoline_base() const;
    // Reuse GraphicThunk's syscall number — both thunks share the
    // __NR_bifrost_thunk dispatcher. The thunk symbol_id namespace uses
    // ID_BASE_AUDIO (0x1000) to avoid collisions with GraphicThunk.
    static constexpr uint64_t SYSCALL_NUMBER = 0x1000;
    static constexpr uint64_t TRAMPOLINE_SIZE = 16;
    static constexpr uint64_t MAX_SYMBOLS = 1024;  // 16 KiB page
    // 1.5.2-alpha: ID base for AudioThunk symbols.
    static constexpr uint32_t ID_BASE = 0x1000;
    static constexpr uint32_t ID_MASK = 0x3000;
private:
    std::unique_ptr<AudioThunkImpl> impl_;
    void register_function_(const std::string& lib,
                            const std::string& sym,
                            void* host_fn);
    void write_trampoline_(Memory& mem, uint64_t addr, uint32_t sym_id);
    void register_known_symbols_();
};
} // namespace arm64emu
