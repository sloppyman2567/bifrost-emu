// OpenAL buffers/sources on AudioEngine streams.
#include "System/Audio/AudioThunk/AudioThunkInternal.hpp"
namespace arm64emu {
#define R(i) (cpu.regs[i])
std::optional<int64_t> AudioDispatch::openal() {
    // ══ OpenAL ══════════════════════════════════════════════════════
    if (name == "alcOpenDevice") { tr(0xA8000001); return 0xA8000001; }
    if (name == "alcCloseDevice") { tr(1); return 1; }
    if (name == "alcCreateContext") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        const uint64_t h = 0xA8100000ull + ++I.al_ctx_;
        tr((int64_t)h); return (int64_t)h;
    }
    if (name == "alcMakeContextCurrent") { tr(1); return 1; }
    if (name == "alcDestroyContext" || name == "alcProcessContext" ||
        name == "alcSuspendContext" || name == "alcGetError" ||
        name == "alGetError") { tr(0); return 0; }
    if (name == "alcGetString" || name == "alGetString") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        static uint64_t abuf = 0;
        if (!abuf) abuf = I.mem->mmap_alloc(128);
        const char* s = "bifrost-emu Software";
        I.mem->write(abuf, s, strlen(s) + 1);
        tr((int64_t)abuf); return (int64_t)abuf;
    }
    if (name == "alcGetIntegerv") {
        uint64_t data = R(3);
        int sz = (int)R(2);
        if (data && sz > 0) {
            std::vector<int32_t> zeros(sz);
            I.mem->write(data, zeros.data(), (size_t)sz * 4);
        }
        tr(0); return 0;
    }
    if (name == "alGenBuffers") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        int n = (int)R(0);
        uint64_t ids = R(1);
        for (int i = 0; i < n && ids; i++) {
            uint64_t id = 0xA9000000ull + ++I.next_al_buf_;
            I.al_bufs_[id] = AudioThunkImpl::AlBuffer{};
            wr32(I, ids + (uint64_t)i * 4, (uint32_t)id);
        }
        tr(0); return 0;
    }
    if (name == "alDeleteBuffers") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        int n = (int)R(0);
        uint64_t ids = R(1);
        for (int i = 0; i < n && ids; i++) {
            uint32_t id = rd32(I, ids + (uint64_t)i * 4);
            I.al_bufs_.erase(id);
        }
        tr(0); return 0;
    }
    if (name == "alBufferData") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        uint32_t bid = (uint32_t)R(0);
        uint32_t alfmt = (uint32_t)R(1);
        uint64_t data = R(2);
        size_t bytes = (size_t)R(3);
        uint32_t rate = (uint32_t)R(4);
        auto it = I.al_bufs_.find(bid);
        if (it != I.al_bufs_.end() && data && bytes) {
            auto& b = it->second;
            b.data.resize(bytes);
            I.mem->read(data, b.data.data(), bytes);
            b.rate = rate ? rate : 44100;
            switch (alfmt) {
                case 0x1100: b.fmt = PCM_FMT_U8;  b.ch = 1; break;
                case 0x1101: b.fmt = PCM_FMT_S16; b.ch = 1; break;
                case 0x1102: b.fmt = PCM_FMT_U8;  b.ch = 2; break;
                case 0x1103: b.fmt = PCM_FMT_S16; b.ch = 2; break;
                case 0x10010: b.fmt = PCM_FMT_F32; b.ch = 1; break;
                case 0x10011: b.fmt = PCM_FMT_F32; b.ch = 2; break;
                default: b.fmt = PCM_FMT_S16; b.ch = 1; break;
            }
        }
        tr(0); return 0;
    }
    if (name == "alGenSources") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        int n = (int)R(0);
        uint64_t ids = R(1);
        for (int i = 0; i < n && ids; i++) {
            uint64_t id = 0xA9800000ull + ++I.next_al_src_;
            I.al_srcs_[id] = AudioThunkImpl::AlSource{};
            wr32(I, ids + (uint64_t)i * 4, (uint32_t)id);
        }
        tr(0); return 0;
    }
    if (name == "alDeleteSources") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        int n = (int)R(0);
        uint64_t ids = R(1);
        for (int i = 0; i < n && ids; i++) {
            auto it = I.al_srcs_.find(rd32(I, ids + (uint64_t)i * 4));
            if (it == I.al_srcs_.end()) continue;
            if (it->second.engine_stream && I.engine)
                I.engine->stream_close(it->second.engine_stream);
            I.al_srcs_.erase(it);
        }
        tr(0); return 0;
    }
    if (name == "alSourceQueueBuffers") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.al_srcs_.find(R(0));
        if (it != I.al_srcs_.end()) {
            int n = (int)R(1);
            uint64_t bufs = R(2);
            for (int i = 0; i < n && bufs; i++)
                it->second.queue.push_back(rd32(I, bufs + (uint64_t)i * 4));
        }
        tr(0); return 0;
    }
    if (name == "alSourcePlay") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.al_srcs_.find(R(0));
        if (it != I.al_srcs_.end() && I.engine) {
            auto& src = it->second;
            src.playing = true;
            for (size_t q = src.played; q < src.queue.size(); q++) {
                auto bit = I.al_bufs_.find(src.queue[q]);
                if (bit == I.al_bufs_.end()) continue;
                // One engine stream per source: concurrent sources MIX
                // in the device callback (real OpenAL semantics).
                if (!src.engine_stream)
                    src.engine_stream = I.engine->stream_open(
                        bit->second.fmt, bit->second.rate, bit->second.ch);
                if (src.engine_stream)
                    I.engine->stream_write(src.engine_stream,
                                           bit->second.fmt, bit->second.rate,
                                           bit->second.ch,
                                           bit->second.data.data(),
                                           bit->second.data.size());
            }
            src.played = src.queue.size();
        }
        tr(0); return 0;
    }
    if (name == "alSourceStop") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.al_srcs_.find(R(0));
        if (it != I.al_srcs_.end()) {
            it->second.queue.clear();
            it->second.played = 0;
            it->second.playing = false;
        }
        tr(0); return 0;
    }
    if (name == "alSourceUnqueueBuffers") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.al_srcs_.find(R(0));
        int processed = 0;
        if (it != I.al_srcs_.end()) {
            int n = (int)R(1);
            uint64_t ids = R(2);
            processed = (int)(it->second.played > (size_t)n ? (size_t)n : it->second.played);
            for (int i = 0; i < processed && ids; i++) {
                wr32(I, ids + (uint64_t)i * 4, (uint32_t)it->second.queue[i]);
            }
            it->second.queue.erase(it->second.queue.begin(),
                                   it->second.queue.begin() + processed);
            it->second.played -= (size_t)processed;
        }
        tr(processed); return processed;
    }
    if (name == "alSourcePause" || name == "alSourceRewind" ||
        name == "alSourcei" || name == "alSourcef" || name == "alSource3f" ||
        name == "alListener3f" || name == "alListenerfv" ||
        name == "alDistanceModel") { tr(0); return 0; }

    return std::nullopt;
}
#undef R
}  // namespace arm64emu
