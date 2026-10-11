// AudioThunk lifecycle, callback pump threads, symbol registration and resolution.
#include "System/Audio/AudioThunk/AudioThunkInternal.hpp"
namespace arm64emu {

// ── AudioThunk lifecycle ───────────────────────────────────────────────
AudioThunk::AudioThunk() {
    impl_ = std::make_unique<AudioThunkImpl>();
    // Enabled by default since 1.5.4; BIFROST_NO_THUNK_AUDIO=1 opts out.
    const char* disable = getenv("BIFROST_NO_THUNK_AUDIO");
    impl_->enabled = !(disable && disable[0] == '1');
    if (impl_->enabled) {
        if (dbg().thunk_trace || getenv("BIFROST_VERBOSE")) {
            fprintf(stderr, "[audio-thunk] audio API thunking enabled "
                    "(SDL2/ALSA/Pulse/OpenAL/AAudio/OpenSL → AudioEngine)\n");
        }
    }
}
AudioThunk::~AudioThunk() = default;
bool AudioThunk::enabled() const { return impl_ && impl_->enabled; }
bool AudioThunk::init(Memory& mem) {
    if (!impl_->enabled) return false;
    if (impl_->initialized) return true;
    std::lock_guard<std::recursive_mutex> g(impl_->mu);
    impl_->mem = &mem;
    impl_->trampoline_base = mem.mmap_alloc(AudioThunkImpl::TRAMPOLINE_PAGE_SIZE);
    if (impl_->trampoline_base == 0) {
        fprintf(stderr, "[audio-thunk] init: failed to allocate trampoline page\n");
        return false;
    }
    register_known_symbols_();
    impl_->initialized = true;
    if (dbg().thunk_trace) {
        fprintf(stderr, "[audio-thunk] init: %zu symbols registered, "
                "trampoline_base=0x%llx\n",
                impl_->id_to_idx_.size(),
                static_cast<unsigned long long>(impl_->trampoline_base));
    }
    return true;
}
void AudioThunk::wire(Audio* engine, AudioCbRunner runner,
                      AudioCbCpuFactory cpu_factory) {
    impl_->engine = engine;
    impl_->runner = std::move(runner);
    impl_->cpu_factory = std::move(cpu_factory);
}
// ── guest-TLS callback worker ──────────────────────────────────────────
void AudioThunk::start_pump() {
    if (!impl_ || !impl_->enabled || !impl_->initialized) return;
    std::lock_guard<std::mutex> start_lock(impl_->pump_mu);
    if (!impl_->runner || !impl_->cpu_factory || !impl_->engine ||
        impl_->pump_enabled) return;
    // Do not create a second guest CPU just because the thunk was wired at
    // process startup. Creating that CPU switches the shared JIT into
    // multithread-safe mode, which disables runtime block chaining and
    // tier-2 compilation even for guests that never use audio. Start the
    // worker only after a callback stream has actually been opened.
    bool has_callback_stream = false;
    for (const auto& [handle, stream] : impl_->sdl_devs_) {
        (void)handle;
        if (stream.cb_fn && stream.bounce && stream.cb_scheduled) {
            has_callback_stream = true;
            break;
        }
    }
    if (!has_callback_stream) {
        for (const auto& [handle, stream] : impl_->aa_streams_) {
            (void)handle;
            if (stream.cb_fn && stream.bounce && stream.cb_scheduled) {
                has_callback_stream = true;
                break;
            }
        }
    }
    if (!has_callback_stream) return;
    const char* mode = getenv("BIFROST_AUDIO_PUMP");
    if (mode && mode[0] == '0') return;  // diagnostic inline fallback
    try {
        impl_->pump_cpu = impl_->cpu_factory();
    } catch (const std::exception& e) {
        if (dbg().thunk_trace)
            fprintf(stderr, "[audio-thunk] guest callback CPU setup failed: %s\n",
                    e.what());
        return;
    }
    if (!impl_->pump_cpu) {
        if (dbg().thunk_trace)
            fprintf(stderr, "[audio-thunk] no independent guest TLS; "
                            "audio callbacks remain inline\n");
        return;
    }
    impl_->pump_enabled = true;
    impl_->pump_go = true;
    impl_->wake_pump();
    auto* raw = impl_.get();
    try { raw->pump_thread = std::thread([raw] {
        auto& I = *raw;
        auto now_us = [] {
            return (uint64_t)std::chrono::duration_cast<
                std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        while (I.pump_go.load(std::memory_order_acquire)) {
            const uint64_t wake_generation =
                I.pump_wake_generation.load(std::memory_order_acquire);
            const uint64_t now = now_us();
            auto wait_for = std::chrono::milliseconds(250);
            uint64_t h = 0, fn = 0, bounce = 0;
            size_t bytes = 0;
            uint32_t fmt = PCM_FMT_S16, rate = 44100;
            uint8_t ch = 2;
            int stream = 0;
            int64_t args[4]{};
            size_t nargs = 0;
            bool aa = false;
            {
                std::lock_guard<std::mutex> lock(I.pump_mu);
                auto scan = [&](auto& devices) {
                    for (auto& [handle, s] : devices) {
                        if (!s.cb_fn || !s.bounce || !s.cb_scheduled || s.guest_thread)
                            continue;
                        if (s.callback_active || s.paused || s.callback_lock_depth)
                            continue;
                        if (now < s.next_cb_us) {
                            const auto until = std::chrono::microseconds(
                                s.next_cb_us - now);
                            wait_for = std::min(wait_for,
                                std::chrono::duration_cast<std::chrono::milliseconds>(until) +
                                std::chrono::milliseconds(1));
                            continue;
                        }
                        h = handle; fn = s.cb_fn; bounce = s.bounce;
                        aa = s.aaudio; fmt = s.fmt; rate = s.rate; ch = s.ch;
                        stream = s.engine_stream;
                        bytes = (size_t)s.frames_per_cb * s.ch * s.size;
                        if (!bytes || !s.rate) { h = 0; continue; }
                        s.callback_active = true;
                        s.callback_host = std::this_thread::get_id();
                        const uint64_t period = std::max<uint64_t>(
                            1000, (uint64_t)s.frames_per_cb * 1000000ULL / s.rate);
                        s.next_cb_us = now + period;
                        if (aa) {
                            args[0] = (int64_t)s.stream_arg;
                            args[1] = (int64_t)s.cb_ud;
                            args[2] = (int64_t)s.bounce;
                            args[3] = (int64_t)s.frames_per_cb; nargs = 4;
                        } else {
                            args[0] = (int64_t)s.cb_ud;
                            args[1] = (int64_t)s.bounce;
                            args[2] = (int64_t)bytes; nargs = 3;
                        }
                        return true;
                    }
                    return false;
                };
                if (!scan(I.sdl_devs_)) scan(I.aa_streams_);
            }
            if (!h) {
                std::unique_lock<std::mutex> wait_lock(I.pump_wait_mu);
                I.pump_cv.wait_for(wait_lock, wait_for, [&] {
                    return !I.pump_go.load(std::memory_order_acquire) ||
                        I.pump_wake_generation.load(std::memory_order_acquire) !=
                            wake_generation;
                });
                continue;
            }
            // SDL initializes each output buffer to the format's silence
            // value before entering the guest callback.
            try {
                std::vector<uint8_t> silence(bytes,
                    fmt == PCM_FMT_U8 ? 128 : 0);
                I.mem->write(bounce, silence.data(), bytes);
            } catch (...) {
                std::lock_guard<std::mutex> lock(I.pump_mu);
                auto si = I.sdl_devs_.find(h);
                auto ai = I.aa_streams_.find(h);
                if (si != I.sdl_devs_.end()) si->second.callback_active = false;
                if (ai != I.aa_streams_.end()) ai->second.callback_active = false;
                I.callback_cv.notify_all();
                I.wake_pump();
                continue;
            }
            uint64_t cb_result = 0;
            try {
                cb_result = I.runner(*I.pump_cpu, fn, args, nargs);
            } catch (...) {
                cb_result = 1;  // fail closed for AAudio; SDL emits silence.
            }
            std::lock_guard<std::mutex> lock(I.pump_mu);
            auto si = I.sdl_devs_.find(h);
            auto ai = I.aa_streams_.find(h);
            auto* live = si != I.sdl_devs_.end() ? &si->second
                       : ai != I.aa_streams_.end() ? &ai->second : nullptr;
            if (!live) continue;
            if (live->close_deferred) {
                if (live->engine_stream && I.engine)
                    I.engine->stream_close(live->engine_stream);
                if (live->bounce)
                    I.mem->untrack_allocation(live->bounce, live->bounce_bytes);
                if (si != I.sdl_devs_.end()) I.sdl_devs_.erase(si);
                else I.aa_streams_.erase(ai);
                I.callback_cv.notify_all();
                I.wake_pump();
                continue;
            }
            if (aa && cb_result != 0) {
                auto it = I.aa_streams_.find(h);
                if (it != I.aa_streams_.end()) {
                    it->second.paused = true;
                    it->second.aa_state = 10;  // AAUDIO_CALLBACK_RESULT_STOP stops
                    it->second.callback_active = false;
                }
                if (stream && I.engine) I.engine->stream_pause(stream, true);
                I.callback_cv.notify_all();
                I.wake_pump();
                continue;
            }
            if (stream && I.engine) {
                try {
                    std::vector<uint8_t> data(bytes);
                    I.mem->read(bounce, data.data(), bytes);
                    I.engine->stream_write(stream, fmt, rate, ch,
                                           data.data(), data.size());
                } catch (...) { /* stream closed or bounce unmapped */ }
            }
            live->callback_active = false;
            I.callback_cv.notify_all();
            I.wake_pump();
        }
    }); } catch (const std::exception& e) {
        impl_->pump_go = false;
        impl_->pump_enabled = false;
        impl_->pump_cpu.reset();
        fprintf(stderr, "[audio-thunk] failed to start callback worker: %s\n",
                e.what());
        return;
    }
    if (dbg().thunk_trace)
        fprintf(stderr, "[audio-thunk] guest-TLS callback worker started\n");
}
// ── fork-child handling ────────────────────────────────────────────────
// Retained for the fork lifecycle interface. No audio callback host thread
// exists; inline callbacks resume naturally if the child returns to guest.
void AudioThunk::detach_pump_for_fork_child() {
    if (!impl_) return;
    impl_->pump_go = false;
    if (impl_->pump_thread.joinable()) impl_->pump_thread.detach();
    impl_->pump_enabled = false;
    impl_->pump_cpu.reset();
    // fork() leaves only the calling host thread in the child. The worker
    // may have owned any pump lock at that instant, so unlocking or
    // destroying an inherited mutex/condition variable is not safe. Start
    // fresh synchronization objects in-place before inline dispatch resumes.
    new (&impl_->pump_mu) std::mutex();
    new (&impl_->pump_wait_mu) std::mutex();
    new (&impl_->callback_cv) std::condition_variable();
    new (&impl_->pump_cv) std::condition_variable();
    for (auto& [handle, stream] : impl_->sdl_devs_) {
        (void)handle;
        stream.callback_active = false;
        stream.callback_host = {};
        stream.close_deferred = false;
    }
    for (auto& [handle, stream] : impl_->aa_streams_) {
        (void)handle;
        stream.callback_active = false;
        stream.callback_host = {};
        stream.close_deferred = false;
    }
}
void AudioThunk::shutdown() {
    if (impl_) impl_->shutdown_pumps();
}
void AudioThunk::register_function_(const std::string& lib,
                                      const std::string& sym,
                                      void* host_fn) {
    bool trace = dbg().thunk_trace;
    thunk_register(*impl_->mem, impl_->libs_, impl_->id_to_idx_,
                   impl_->trampoline_base, TRAMPOLINE_SIZE, MAX_SYMBOLS,
                   static_cast<uint16_t>(SYSCALL_NUMBER),
                   AudioThunk::ID_BASE, trace,
                   lib, sym, host_fn);
}
void AudioThunk::write_trampoline_(Memory& mem, uint64_t addr, uint32_t sym_id) {
    write_thunk_trampoline(mem, addr, sym_id,
                            static_cast<uint16_t>(SYSCALL_NUMBER));
}
uint64_t AudioThunk::resolve(const std::string& lib, const std::string& sym) {
    if (!impl_ || !impl_->enabled || !impl_->initialized) return 0;
    std::lock_guard<std::recursive_mutex> g(impl_->mu);
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
    std::lock_guard<std::recursive_mutex> g(impl_->mu);
    auto* lt = find_lib(const_cast<std::vector<ThunkLibTable>&>(impl_->libs_), lib);
    if (!lt) return 0;
    for (const auto& e : lt->entries) {
        cb(e.name, e.guest_addr);
    }
    return lt->entries.size();
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
// Every row resolves to a trampoline; dispatch() routes ALL of them to
// AudioEngine arms (host audio libs are never called). Rows still exist for
// symbols guests dlsym at startup so resolution never fails. Synthetic
// OpenSL method names (__osl_*) build the guest vtables; SL_IID_* rows are
// DATA stand-ins whose trampoline addresses double as IID identity tags.
void AudioThunk::register_known_symbols_() {
    // ── libasound.so.2 (ALSA) ──────────────────────────────────────
    const char* alsa_libs[] = {"libasound.so.2", "libasound.so"};
    #define REG(libs, name) \
        for (const char* L : libs) register_function_(L, #name, nullptr)
    REG(alsa_libs, snd_pcm_open);
    REG(alsa_libs, snd_pcm_close);
    REG(alsa_libs, snd_pcm_hw_params_malloc);
    REG(alsa_libs, snd_pcm_hw_params_free);
    REG(alsa_libs, snd_pcm_hw_params_any);
    REG(alsa_libs, snd_pcm_hw_params_set_access);
    REG(alsa_libs, snd_pcm_hw_params_set_format);
    REG(alsa_libs, snd_pcm_hw_params_set_channels);
    REG(alsa_libs, snd_pcm_hw_params_set_rate);
    REG(alsa_libs, snd_pcm_hw_params);
    REG(alsa_libs, snd_pcm_writei);
    REG(alsa_libs, snd_pcm_readi);
    REG(alsa_libs, snd_pcm_drain);
    REG(alsa_libs, snd_pcm_drop);
    REG(alsa_libs, snd_pcm_pause);
    REG(alsa_libs, snd_pcm_recover);
    REG(alsa_libs, snd_pcm_prepare);
    REG(alsa_libs, snd_pcm_start);
    REG(alsa_libs, snd_strerror);
    REG(alsa_libs, snd_pcm_info);
    REG(alsa_libs, snd_pcm_avail_update);
    REG(alsa_libs, snd_pcm_delay);
    // ── libpulse.so.0 (PulseAudio simple) ─────────────────────────
    const char* pulse_libs[] = {"libpulse.so.0", "libpulse.so"};
    REG(pulse_libs, pa_simple_new);
    REG(pulse_libs, pa_simple_write);
    REG(pulse_libs, pa_simple_drain);
    REG(pulse_libs, pa_simple_free);
    REG(pulse_libs, pa_simple_flush);
    REG(pulse_libs, pa_simple_get_latency);
    REG(pulse_libs, pa_strerror);
    // ── libSDL2 (audio subset) ────────────────────────────────────
    const char* sdl_libs[] = {"libSDL2.so", "libSDL2-2.0.so.0"};
    REG(sdl_libs, SDL_OpenAudioDevice);
    REG(sdl_libs, SDL_OpenAudio);
    REG(sdl_libs, SDL_CloseAudioDevice);
    REG(sdl_libs, SDL_PauseAudioDevice);
    REG(sdl_libs, SDL_PauseAudio);
    REG(sdl_libs, SDL_LockAudioDevice);
    REG(sdl_libs, SDL_UnlockAudioDevice);
    REG(sdl_libs, SDL_QueueAudio);
    REG(sdl_libs, SDL_DequeueAudio);
    REG(sdl_libs, SDL_GetQueuedAudioSize);
    REG(sdl_libs, SDL_ClearQueuedAudio);
    REG(sdl_libs, SDL_AudioInit);
    REG(sdl_libs, SDL_AudioQuit);
    REG(sdl_libs, SDL_MixAudioFormat);
    REG(sdl_libs, SDL_LockAudio);
    REG(sdl_libs, SDL_UnlockAudio);
    REG(sdl_libs, SDL_CloseAudio);
    REG(sdl_libs, SDL_GetCurrentAudioDriver);
    REG(sdl_libs, SDL_GetNumAudioDevices);
    REG(sdl_libs, SDL_GetAudioDeviceName);
    REG(sdl_libs, SDL_GetAudioDeviceSpec);
    // ── libopenal ─────────────────────────────────────────────────
    const char* openal_libs[] = {"libopenal.so.1", "libopenal.so"};
    REG(openal_libs, alcOpenDevice);
    REG(openal_libs, alcCloseDevice);
    REG(openal_libs, alcCreateContext);
    REG(openal_libs, alcMakeContextCurrent);
    REG(openal_libs, alcDestroyContext);
    REG(openal_libs, alcProcessContext);
    REG(openal_libs, alcSuspendContext);
    REG(openal_libs, alcGetError);
    REG(openal_libs, alcGetString);
    REG(openal_libs, alcGetIntegerv);
    REG(openal_libs, alGenSources);
    REG(openal_libs, alDeleteSources);
    REG(openal_libs, alSourcePlay);
    REG(openal_libs, alSourceStop);
    REG(openal_libs, alSourcePause);
    REG(openal_libs, alSourceRewind);
    REG(openal_libs, alSourceQueueBuffers);
    REG(openal_libs, alSourceUnqueueBuffers);
    REG(openal_libs, alGenBuffers);
    REG(openal_libs, alDeleteBuffers);
    REG(openal_libs, alBufferData);
    REG(openal_libs, alGetError);
    REG(openal_libs, alGetString);
    REG(openal_libs, alListener3f);
    REG(openal_libs, alListenerfv);
    REG(openal_libs, alSource3f);
    REG(openal_libs, alSourcef);
    REG(openal_libs, alSourcei);
    REG(openal_libs, alDistanceModel);
    // ── libaaudio.so (Android) ────────────────────────────────────
    const char* aa_libs[] = {"libaaudio.so"};
    REG(aa_libs, AAudioStreamBuilder_new);
    REG(aa_libs, AAudio_createStreamBuilder);
    REG(aa_libs, AAudioStreamBuilder_setDeviceId);
    REG(aa_libs, AAudioStreamBuilder_setDirection);
    REG(aa_libs, AAudioStreamBuilder_setSharingMode);
    REG(aa_libs, AAudioStreamBuilder_setPerformanceMode);
    REG(aa_libs, AAudioStreamBuilder_setUsage);
    REG(aa_libs, AAudioStreamBuilder_setInputPreset);
    REG(aa_libs, AAudioStreamBuilder_setSessionId);
    REG(aa_libs, AAudioStreamBuilder_setBufferCapacityInFrames);
    REG(aa_libs, AAudioStream_getBufferCapacityInFrames);
    REG(aa_libs, AAudioStream_setBufferSizeInFrames);
    REG(aa_libs, AAudioStream_getDeviceId);
    REG(aa_libs, AAudioStream_getSessionId);
    REG(aa_libs, AAudioStream_getUsage);
    REG(aa_libs, AAudioStream_getInputPreset);
    REG(aa_libs, AAudioStream_getTimestamp);
    REG(aa_libs, AAudioStream_getXRunCount);
    REG(aa_libs, AAudioStream_isMMapUsed);
    REG(aa_libs, AAudioStream_getSharingMode);
    REG(aa_libs, AAudioStream_getPerformanceMode);
    REG(aa_libs, AAudioStream_getDirection);
    // Private: the Android adapter's guest callback thread (aaudio.c).
    REG(aa_libs, bifrost_aaudio_claim_callback);
    REG(aa_libs, bifrost_aaudio_wait_callback);
    REG(aa_libs, bifrost_aaudio_finish_callback);
    REG(aa_libs, AAudioStreamBuilder_delete);
    REG(aa_libs, AAudioStreamBuilder_setFormat);
    REG(aa_libs, AAudioStreamBuilder_setChannelCount);
    REG(aa_libs, AAudioStreamBuilder_setSampleRate);
    REG(aa_libs, AAudioStreamBuilder_setFramesPerDataCallback);
    REG(aa_libs, AAudioStreamBuilder_setDataCallback);
    REG(aa_libs, AAudioStreamBuilder_setErrorCallback);
    REG(aa_libs, AAudioStreamBuilder_openStream);
    REG(aa_libs, AAudioStream_requestStart);
    REG(aa_libs, AAudioStream_requestPause);
    REG(aa_libs, AAudioStream_requestStop);
    REG(aa_libs, AAudioStream_close);
    REG(aa_libs, AAudioStream_write);
    REG(aa_libs, AAudioStream_read);
    REG(aa_libs, AAudioStream_getState);
    REG(aa_libs, AAudioStream_waitForStateChange);
    REG(aa_libs, AAudioStream_getFormat);
    REG(aa_libs, AAudioStream_getChannelCount);
    REG(aa_libs, AAudioStream_getSampleRate);
    REG(aa_libs, AAudioStream_getBufferSizeInFrames);
    REG(aa_libs, AAudioStream_getFramesPerBurst);
    REG(aa_libs, AAudioStream_getFramesPerDataCallback);
    REG(aa_libs, AAudioStream_getFramesRead);
    REG(aa_libs, AAudioStream_getFramesWritten);
    REG(aa_libs, AAudio_convertResultToText);
    // ── libOpenSLES.so (Android) — synthetic vtable methods + IIDs ─
    const char* sl_libs[] = {"libOpenSLES.so"};
    REG(sl_libs, slCreateEngine);
    REG(sl_libs, __osl_realize);
    REG(sl_libs, __osl_destroy);
    REG(sl_libs, __osl_getinterface);
    REG(sl_libs, __osl_eng_createmix);
    REG(sl_libs, __osl_eng_createplayer);
    REG(sl_libs, __osl_bq_enqueue);
    REG(sl_libs, __osl_bq_register);
    REG(sl_libs, __osl_play_setstate);
    REG(sl_libs, sl_IID_ENGINE_dummy);   // placeholder keeps count stable
    #undef REG
    // IID blobs: register under their real exported names; dispatch keys
    // off the returned trampoline addresses (see sl_iids_ in dispatch).
    struct IidRow { const char* sym; uint64_t tag; };
    const IidRow iids[] = {
        {"SL_IID_ENGINE", 0}, {"SL_IID_OUTPUTMIX", 1},
        {"SL_IID_BUFFERQUEUE", 2}, {"SL_IID_PLAY", 3},
        {"SL_IID_ANDROIDCONFIGURATION", 4},
    };
    for (const auto& r : iids) {
        register_function_("libOpenSLES.so", r.sym, nullptr);
        // find its freshly-assigned trampoline address and record the tag
        if (auto* lt = find_lib(impl_->libs_, "libOpenSLES.so")) {
            for (const auto& e : lt->entries)
                if (e.name == r.sym && !impl_->sl_iids_.count(e.guest_addr))
                    impl_->sl_iids_[e.guest_addr] = r.tag;
        }
    }
}
}  // namespace arm64emu
