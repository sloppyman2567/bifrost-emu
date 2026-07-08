// frost_graphics/audio_thunk.cpp — AudioThunk implementation (v1.5.0.alpha).
//
// See include/frost/audio_thunk.hpp for the design overview. This file
// implements the AudioThunk class using the shared thunk_common helpers.
#include "frost/audio_thunk.hpp"
#include "frost/thunk.hpp"  // for SYSCALL_NUMBER constant
#include "thunk_common.hpp"

#include <dlfcn.h>
#include <mutex>
#include <string>

namespace arm64emu {

struct AudioThunkImpl {
    bool   enabled = false;
    Memory* mem    = nullptr;
    bool   initialized = false;

    uint64_t trampoline_base = 0;
    static constexpr uint64_t TRAMPOLINE_PAGE_SIZE =
        AudioThunk::TRAMPOLINE_SIZE * AudioThunk::MAX_SYMBOLS;  // 16 KiB

    std::vector<ThunkLibTable> libs_;
    std::vector<std::pair<uint32_t, uint32_t>> id_to_idx_;

    std::mutex mu;
};

// ── AudioThunk lifecycle ───────────────────────────────────────────────
AudioThunk::AudioThunk() {
    impl_ = std::make_unique<AudioThunkImpl>();
    impl_->enabled = (getenv("BIFROST_THUNK_AUDIO") != nullptr);
    if (impl_->enabled) {
        fprintf(stderr, "[audio-thunk] audio API thunking enabled "
                "(EXPERIMENTAL, partial ALSA/PulseAudio/SDL2/OpenAL support)\n");
    }
}

AudioThunk::~AudioThunk() = default;

bool AudioThunk::enabled() const { return impl_ && impl_->enabled; }

bool AudioThunk::init(Memory& mem) {
    if (!impl_->enabled) return false;
    if (impl_->initialized) return true;

    std::lock_guard<std::mutex> g(impl_->mu);
    impl_->mem = &mem;
    impl_->trampoline_base = mem.mmap_alloc(AudioThunkImpl::TRAMPOLINE_PAGE_SIZE);
    if (impl_->trampoline_base == 0) {
        fprintf(stderr, "[audio-thunk] init: failed to allocate trampoline page\n");
        return false;
    }
    register_known_symbols_();
    impl_->initialized = true;

    if (getenv("BIFROST_THUNK_TRACE")) {
        fprintf(stderr, "[audio-thunk] init: %zu symbols registered, "
                "trampoline_base=0x%llx\n",
                impl_->id_to_idx_.size(),
                static_cast<unsigned long long>(impl_->trampoline_base));
    }
    return true;
}

void AudioThunk::register_function_(const std::string& lib,
                                      const std::string& sym,
                                      void* host_fn) {
    bool trace = (getenv("BIFROST_THUNK_TRACE") != nullptr);
    thunk_register(*impl_->mem, impl_->libs_, impl_->id_to_idx_,
                   impl_->trampoline_base, TRAMPOLINE_SIZE, MAX_SYMBOLS,
                   static_cast<uint16_t>(SYSCALL_NUMBER), trace,
                   lib, sym, host_fn);
}

void AudioThunk::write_trampoline_(Memory& mem, uint64_t addr, uint32_t sym_id) {
    write_thunk_trampoline(mem, addr, sym_id,
                            static_cast<uint16_t>(SYSCALL_NUMBER));
}

uint64_t AudioThunk::resolve(const std::string& lib, const std::string& sym) {
    if (!impl_ || !impl_->enabled || !impl_->initialized) return 0;
    std::lock_guard<std::mutex> g(impl_->mu);
    auto* lt = find_lib(impl_->libs_, lib);
    if (!lt) return 0;
    for (const auto& e : lt->entries) {
        if (e.name == sym) return e.guest_addr;
    }
    return 0;
}

size_t AudioThunk::enumerate_symbols(const std::string& lib,
    const std::function<void(const std::string&, uint64_t)>& cb) const {
    if (!impl_ || !impl_->enabled || !impl_->initialized) return 0;
    std::lock_guard<std::mutex> g(impl_->mu);
    auto* lt = find_lib(const_cast<std::vector<ThunkLibTable>&>(impl_->libs_), lib);
    if (!lt) return 0;
    for (const auto& e : lt->entries) {
        cb(e.name, e.guest_addr);
    }
    return lt->entries.size();
}

int64_t AudioThunk::dispatch(CPU& cpu, uint32_t symbol_id) {
    if (!impl_ || !impl_->enabled || !impl_->initialized) {
        return -ENOSYS;
    }
    if (symbol_id >= impl_->id_to_idx_.size()) {
        return -ENOENT;
    }
    auto [lib_idx, ent_idx] = impl_->id_to_idx_[symbol_id];
    const auto& entry = impl_->libs_[lib_idx].entries[ent_idx];
    bool trace = (getenv("BIFROST_THUNK_TRACE") != nullptr);
    return thunk_dispatch_generic(cpu, entry.host_fn, entry.name, trace);
}

size_t AudioThunk::symbol_count() const {
    if (!impl_) return 0;
    return impl_->id_to_idx_.size();
}

uint64_t AudioThunk::trampoline_base() const {
    if (!impl_) return 0;
    return impl_->trampoline_base;
}

// ── register_known_symbols_ ────────────────────────────────────────────
// Populate the registry with the audio entry points we know how to
// thunk. Each entry maps a (library, symbol) pair to the host function
// pointer (when the host has the dev headers and the library is
// loadable via dlsym) or to a null stub (when we want the symbol to
// resolve so the guest doesn't fail at dlsym time, but we don't have a
// real implementation).
//
// We use dlsym(RTLD_DEFAULT, ...) to find the host function. If the
// host doesn't have the library installed, dlsym returns nullptr and
// the symbol is registered as a stub.
void AudioThunk::register_known_symbols_() {
    // ── libasound.so.2 (ALSA) ──────────────────────────────────────
    const char* alsa_libs[] = {"libasound.so.2", "libasound.so"};
    void* alsa_handle = dlopen("libasound.so.2", RTLD_LAZY);
    if (!alsa_handle) alsa_handle = dlopen("libasound.so", RTLD_LAZY);
    #define REG_ALSA(name) do { \
        void* p = alsa_handle ? dlsym(alsa_handle, #name) : nullptr; \
        for (const char* L : alsa_libs) register_function_(L, #name, p); \
    } while(0)

    REG_ALSA(snd_pcm_open);
    REG_ALSA(snd_pcm_close);
    REG_ALSA(snd_pcm_hw_params_malloc);
    REG_ALSA(snd_pcm_hw_params_free);
    REG_ALSA(snd_pcm_hw_params_any);
    REG_ALSA(snd_pcm_hw_params_set_access);
    REG_ALSA(snd_pcm_hw_params_set_format);
    REG_ALSA(snd_pcm_hw_params_set_channels);
    REG_ALSA(snd_pcm_hw_params_set_rate);
    REG_ALSA(snd_pcm_hw_params);
    REG_ALSA(snd_pcm_writei);
    REG_ALSA(snd_pcm_readi);
    REG_ALSA(snd_pcm_drain);
    REG_ALSA(snd_pcm_drop);
    REG_ALSA(snd_pcm_pause);
    REG_ALSA(snd_pcm_recover);
    REG_ALSA(snd_strerror);
    REG_ALSA(snd_pcm_info);
    REG_ALSA(snd_pcm_avail_update);
    REG_ALSA(snd_pcm_delay);
    #undef REG_ALSA

    // ── libpulse.so.0 (PulseAudio) ────────────────────────────────
    const char* pulse_libs[] = {"libpulse.so.0", "libpulse.so"};
    void* pulse_handle = dlopen("libpulse.so.0", RTLD_LAZY);
    if (!pulse_handle) pulse_handle = dlopen("libpulse.so", RTLD_LAZY);
    #define REG_PULSE(name) do { \
        void* p = pulse_handle ? dlsym(pulse_handle, #name) : nullptr; \
        for (const char* L : pulse_libs) register_function_(L, #name, p); \
    } while(0)

    REG_PULSE(pa_simple_new);
    REG_PULSE(pa_simple_write);
    REG_PULSE(pa_simple_drain);
    REG_PULSE(pa_simple_free);
    REG_PULSE(pa_simple_flush);
    REG_PULSE(pa_simple_get_latency);
    REG_PULSE(pa_strerror);
    REG_PULSE(pa_threaded_mainloop_new);
    REG_PULSE(pa_threaded_mainloop_free);
    REG_PULSE(pa_threaded_mainloop_start);
    REG_PULSE(pa_threaded_mainloop_stop);
    REG_PULSE(pa_threaded_mainloop_lock);
    REG_PULSE(pa_threaded_mainloop_unlock);
    REG_PULSE(pa_threaded_mainloop_wait);
    #undef REG_PULSE

    // ── libSDL2.so (audio subset) ─────────────────────────────────
    // Most SDL2 audio functions take integer/handle args and are
    // well-suited to thunking. Pointer-arg functions (SDL_AudioSpec)
    // need guest→host translation; for now we stub them.
    const char* sdl_libs[] = {"libSDL2.so", "libSDL2-2.0.so.0"};
    void* sdl_handle = dlopen("libSDL2-2.0.so.0", RTLD_LAZY);
    if (!sdl_handle) sdl_handle = dlopen("libSDL2.so", RTLD_LAZY);
    #define REG_SDL(name) do { \
        void* p = sdl_handle ? dlsym(sdl_handle, #name) : nullptr; \
        for (const char* L : sdl_libs) register_function_(L, #name, p); \
    } while(0)

    REG_SDL(SDL_OpenAudioDevice);
    REG_SDL(SDL_CloseAudioDevice);
    REG_SDL(SDL_PauseAudioDevice);
    REG_SDL(SDL_LockAudioDevice);
    REG_SDL(SDL_UnlockAudioDevice);
    REG_SDL(SDL_QueueAudio);
    REG_SDL(SDL_DequeueAudio);
    REG_SDL(SDL_GetQueuedAudioSize);
    REG_SDL(SDL_ClearQueuedAudio);
    REG_SDL(SDL_AudioInit);
    REG_SDL(SDL_AudioQuit);
    REG_SDL(SDL_MixAudioFormat);
    REG_SDL(SDL_GetNumAudioDevices);
    REG_SDL(SDL_GetAudioDeviceName);
    REG_SDL(SDL_GetAudioDeviceSpec);
    #undef REG_SDL

    // ── libopenal.so.1 (OpenAL) ───────────────────────────────────
    const char* openal_libs[] = {"libopenal.so.1", "libopenal.so"};
    void* openal_handle = dlopen("libopenal.so.1", RTLD_LAZY);
    if (!openal_handle) openal_handle = dlopen("libopenal.so", RTLD_LAZY);
    #define REG_OPENAL(name) do { \
        void* p = openal_handle ? dlsym(openal_handle, #name) : nullptr; \
        for (const char* L : openal_libs) register_function_(L, #name, p); \
    } while(0)

    REG_OPENAL(alcOpenDevice);
    REG_OPENAL(alcCloseDevice);
    REG_OPENAL(alcCreateContext);
    REG_OPENAL(alcMakeContextCurrent);
    REG_OPENAL(alcDestroyContext);
    REG_OPENAL(alcProcessContext);
    REG_OPENAL(alcSuspendContext);
    REG_OPENAL(alcGetError);
    REG_OPENAL(alcGetString);
    REG_OPENAL(alcGetIntegerv);
    REG_OPENAL(alGenSources);
    REG_OPENAL(alDeleteSources);
    REG_OPENAL(alSourcePlay);
    REG_OPENAL(alSourceStop);
    REG_OPENAL(alSourcePause);
    REG_OPENAL(alSourceRewind);
    REG_OPENAL(alSourceQueueBuffers);
    REG_OPENAL(alSourceUnqueueBuffers);
    REG_OPENAL(alGenBuffers);
    REG_OPENAL(alDeleteBuffers);
    REG_OPENAL(alBufferData);
    REG_OPENAL(alGetError);
    REG_OPENAL(alGetString);
    REG_OPENAL(alListener3f);
    REG_OPENAL(alListenerfv);
    REG_OPENAL(alSource3f);
    REG_OPENAL(alSourcef);
    REG_OPENAL(alSourcei);
    #undef REG_OPENAL
}

} // namespace arm64emu
