// frost_graphics/audio_thunk.cpp — AudioThunk implementation (1.5.5-alpha).
//
// See include/frost/audio_thunk.hpp for the design overview.
//
// 1.5.5-alpha REWRITE ("working audio path"): guest audio API calls are NO
// LONGER forwarded to same-name host libraries (host libasound/libpulse
// deref opaque structs expecting HOST pointers — a dead end that forced the
// old build to stub everything). Instead every arm converts its call into
// plain sample pushes on the shared AudioEngine ring via
// Audio::write_interleaved() — exactly what /dev/dsp does today. One mixer,
// many front doors:
//
//   SDL2 queue+callback | ALSA subset | Pulse simple | OpenAL state machine
//   | AAudio builder/streams | OpenSL ES synthetic vtables → AudioEngine
//
// Guest callbacks NEVER cross to host libs: they are stored and fired
// through the borrow-CPU runner (Emulator::call_guest_function pattern)
// from per-stream pump threads, filling a direct-window bounce buffer
// (Memory::mmap_alloc — JIT-fast, guest-derefable).
#include "frost/audio_thunk.hpp"
#include "audio/audio.h"
#include "frost/thunk.hpp"  // for SYSCALL_NUMBER constant
#include "thunk_common.hpp"
#include "debug_flags.h"    // dbg() — cached trace gates (BIFROST_THUNK_TRACE)
#include "core/memory.h"
#include <dlfcn.h>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <chrono>
#include <thread>
namespace arm64emu {

struct AudioThunkImpl {
    bool   enabled = false;
    bool   initialized = false;
    Memory* mem    = nullptr;
    Audio*  engine = nullptr;          // borrowed (owned by Emulator)
    AudioCbRunner runner;              // may be empty → callbacks dropped
    CPU*    cb_cpu = nullptr;          // borrowed main CPU for the runner
    uint64_t trampoline_base = 0;
    static constexpr uint64_t TRAMPOLINE_PAGE_SIZE =
        AudioThunk::TRAMPOLINE_SIZE * AudioThunk::MAX_SYMBOLS;  // 16 KiB
    std::vector<ThunkLibTable> libs_;
    std::vector<std::pair<uint32_t, uint32_t>> id_to_idx_;
    std::mutex mu;

    // ── SDL2 devices ──────────────────────────────────────────────────
    struct PumpStream {
        uint32_t fmt = PCM_FMT_S16;
        uint32_t rate = 44100;
        uint8_t ch = 2;
        uint8_t size = 2;
        uint64_t cb_fn = 0, cb_ud = 0;      // guest callback (0 = queue mode)
        uint64_t bounce = 0;                 // direct-window bounce
        size_t bounce_bytes = 0;
        uint32_t frames_per_cb = 1024;
        int64_t stream_arg = 0;              // AAudio: first cb arg (stream)
        bool aaudio = false;                 // AAudio cb signature vs SDL2
        std::thread pump;
        std::atomic<bool> stop{false};
        std::atomic<bool> paused{false};
    };
    std::map<uint64_t, PumpStream> sdl_devs_;
    std::map<uint64_t, PumpStream> aa_streams_;

    // ── ALSA pcm handles ──────────────────────────────────────────────
    struct AlsaPcm { uint32_t fmt = PCM_FMT_S16; uint32_t rate = 44100; uint8_t ch = 2; };
    std::map<uint64_t, AlsaPcm> alsa_pcms_;

    // ── Pulse simple streams ──────────────────────────────────────────
    struct PulseSimple { uint32_t fmt = PCM_FMT_S16; uint32_t rate = 44100; uint8_t ch = 2; };
    std::map<uint64_t, PulseSimple> pulse_streams_;

    // ── OpenAL state machine ──────────────────────────────────────────
    struct AlBuffer {
        std::vector<uint8_t> data;
        uint32_t fmt = PCM_FMT_S16;
        uint32_t rate = 44100;
        uint8_t ch = 1;
    };
    struct AlSource {
        std::vector<uint64_t> queue;       // queued buffer ids
        size_t played = 0;                 // buffers consumed by play()
        bool playing = false;
    };
    std::map<uint64_t, AlBuffer> al_bufs_;
    std::map<uint64_t, AlSource> al_srcs_;
    uint64_t al_ctx_ = 0;

    // ── AAudio builders ───────────────────────────────────────────────
    struct AaBuilder {
        int32_t fmt = 0;                   // AAUDIO_FORMAT_UNSPECIFIED=0/I16=1/FLOAT=2
        int32_t ch = 0, rate = 0;
        uint64_t cb_fn = 0, cb_ud = 0, err_fn = 0, err_ud = 0;
    };
    std::map<uint64_t, AaBuilder> aa_builders_;

    // ── OpenSL ES objects/interfaces (synthetic vtables in guest RAM) ─
    struct SlObject {
        uint64_t itf_word = 0;             // address of the itf-pointer word
        uint64_t kind = 0;                 // 0=engine,1=outputmix,2=player
        bool realized = false;
        uint64_t bq_itf_word = 0, play_itf_word = 0;
        uint64_t bq_cb_fn = 0, bq_cb_ctx = 0;
    };
    std::vector<std::unique_ptr<SlObject>> sl_objs_;      // owned
    std::map<uint64_t, SlObject*> sl_by_word_;            // itf word → object
    std::map<uint64_t, SlObject*> sl_bq_by_word_;         // bufferqueue word → object
    std::map<uint64_t, SlObject*> sl_play_by_word_;       // play itf word → object
    std::map<uint64_t, SlObject*> sl_eng_by_word_;        // engine-itf word → object
    std::map<uint64_t, uint64_t> sl_iids_;                // IID blob guest addr → tag

    ~AudioThunkImpl() { shutdown_pumps(); }
    void shutdown_pumps() {
        for (auto& [h, d] : sdl_devs_) {
            d.stop = true;
            if (d.pump.joinable()) d.pump.join();
        }
        for (auto& [h, s] : aa_streams_) {
            s.stop = true;
            if (s.pump.joinable()) s.pump.join();
        }
    }
};

// ── small guest-memory helpers ─────────────────────────────────────────
namespace {
inline uint64_t rd64(AudioThunkImpl& I, uint64_t a) {
    uint64_t v = 0; I.mem->read(a, &v, sizeof(v)); return v;
}
inline uint32_t rd32(AudioThunkImpl& I, uint64_t a) {
    uint32_t v = 0; I.mem->read(a, &v, sizeof(v)); return v;
}
inline uint16_t rd16(AudioThunkImpl& I, uint64_t a) {
    uint16_t v = 0; I.mem->read(a, &v, sizeof(v)); return v;
}
inline void wr64(AudioThunkImpl& I, uint64_t a, uint64_t v) {
    I.mem->write(a, &v, sizeof(v));
}
inline void wr32(AudioThunkImpl& I, uint64_t a, uint32_t v) {
    I.mem->write(a, &v, sizeof(v));
}
std::string rd_cstr(AudioThunkImpl& I, uint64_t a) {
    std::string s;
    if (!a) return s;
    char c = 0; size_t n = 0;
    while (n < 256) {
        I.mem->read(a + n, &c, 1);
        if (!c) break;
        s += c; ++n;
    }
    return s;
}
}  // namespace

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
    std::lock_guard<std::mutex> g(impl_->mu);
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
void AudioThunk::wire(Audio* engine, CPU* cb_cpu, AudioCbRunner runner) {
    impl_->engine = engine;
    impl_->cb_cpu = cb_cpu;
    impl_->runner = std::move(runner);
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
size_t AudioThunk::symbol_count() const {
    if (!impl_) return 0;
    return impl_->id_to_idx_.size();
}
uint64_t AudioThunk::trampoline_base() const {
    if (!impl_) return 0;
    return impl_->trampoline_base;
}


// ── dispatch ───────────────────────────────────────────────────────────
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
    auto tr = [&](int64_t r) {
        if (trace) fprintf(stderr, "[audio-thunk] %s → %lld\n",
                           name.c_str(), (long long)r);
    };
    #define R(i) (cpu.regs[i])

    // Shared callback pump worker: fires the GUEST callback via the
    // borrow-CPU runner into the direct-window bounce, then pushes the
    // produced bytes onto the engine ring. Runs on a host std::thread.
    auto pump_loop = [&I](AudioThunkImpl::PumpStream& s) {
        while (!s.stop.load(std::memory_order_relaxed)) {
            if (!s.paused.load(std::memory_order_relaxed) &&
                I.runner && I.cb_cpu && s.cb_fn && s.bounce) {
                int64_t ia[4];
                size_t na;
                if (s.aaudio) {
                    ia[0] = (int64_t)s.stream_arg;
                    ia[1] = (int64_t)s.cb_ud;
                    ia[2] = (int64_t)s.bounce;
                    ia[3] = (int64_t)s.frames_per_cb;
                    na = 4;
                } else {  // SDL2: (userdata, stream, len)
                    ia[0] = (int64_t)s.cb_ud;
                    ia[1] = (int64_t)s.bounce;
                    ia[2] = (int64_t)(s.frames_per_cb * s.ch * s.size);
                    na = 3;
                }
                I.runner(*I.cb_cpu, s.cb_fn, ia, na);
                size_t want = s.frames_per_cb * s.ch * s.size;
                std::vector<uint8_t> tmp(want);
                I.mem->read(s.bounce, tmp.data(), want);
                if (I.engine) {
                    I.engine->write_interleaved(s.fmt, s.rate, s.ch,
                                                tmp.data(), want);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    auto sdl_fmt_map = [](uint16_t f) -> uint32_t {
        switch (f & 0xFF1F) {
            case 0x0008: return PCM_FMT_U8;
            case 0x0020: case 0x8020: return PCM_FMT_S16;  // S32 → narrow
            case 0x0120: case 0x8120: case 0x9120: return PCM_FMT_F32;
            case 0x8030: return PCM_FMT_S24;
            default: return PCM_FMT_S16;                   // S16 variants
        }
    };
    auto fmt_size = [](uint32_t f) -> uint8_t {
        switch (f) {
            case PCM_FMT_U8: return 1;
            case PCM_FMT_F32: return 4;
            default: return 2;   // S16 + S24-in-32
        }
    };

    // ══ SDL2 ════════════════════════════════════════════════════════
    // Guest SDL_AudioSpec layout (AAPCS64): freq@0 i32, format@4 u16,
    // channels@6 u8, silence@7 u8, samples@8 u16, padding@10 u16,
    // size@12 u32, pad@14, callback@16 ptr, userdata@24 ptr. Size 32.
    if (name == "SDL_OpenAudioDevice" || name == "SDL_OpenAudio") {
        const bool is_dev = (name == "SDL_OpenAudioDevice");
        const uint64_t desired  = is_dev ? R(2) : R(0);
        const uint64_t obtained = is_dev ? R(3) : R(1);
        if (!desired || !I.engine) { tr(-1); return -1; }
        const int32_t  freq = (int32_t)rd32(I, desired + 0);
        const uint16_t ftag = rd16(I, desired + 4);
        uint8_t chb = 0; I.mem->read(desired + 6, &chb, 1);
        const uint8_t ch = chb ? chb : 2;
        const uint64_t cb_fn = rd64(I, desired + 16);
        const uint64_t cb_ud = rd64(I, desired + 24);
        const uint32_t fmt = sdl_fmt_map(ftag);
        const uint8_t  sz  = fmt_size(fmt);
        const uint64_t h = 0xA5000000ull + I.sdl_devs_.size() + 1;
        auto& slot = I.sdl_devs_[h];
        slot.fmt = fmt; slot.rate = (freq > 0) ? (uint32_t)freq : 44100;
        slot.ch = ch; slot.size = sz;
        slot.cb_fn = cb_fn; slot.cb_ud = cb_ud;
        if (cb_fn) {
            slot.bounce_bytes = (size_t)slot.frames_per_cb * slot.ch * slot.size;
            slot.bounce = I.mem->mmap_alloc(slot.bounce_bytes);
            if (!slot.bounce) { I.sdl_devs_.erase(h); tr(-ENOMEM); return -ENOMEM; }
            slot.pump = std::thread([&I, h, pump_loop] {
                auto it = I.sdl_devs_.find(h);   // map node stable until erase
                if (it == I.sdl_devs_.end()) return;
                pump_loop(it->second);
            });
        }
        if (obtained) {
            wr32(I, obtained + 0, slot.rate);
            I.mem->write(obtained + 4, &ftag, 2);
            I.mem->write(obtained + 6, &slot.ch, 1);
            uint8_t silence = (fmt == PCM_FMT_U8) ? 128 : 0;
            I.mem->write(obtained + 7, &silence, 1);
            uint16_t samples = 1024;
            I.mem->write(obtained + 8, &samples, 2);
            uint16_t padw = 0; I.mem->write(obtained + 10, &padw, 2);
            wr32(I, obtained + 12,
                 (uint32_t)(slot.frames_per_cb * slot.ch * slot.size));
            wr64(I, obtained + 16, cb_fn);
            wr64(I, obtained + 24, cb_ud);
        }
        I.engine->open(slot.rate, slot.ch, sz);
        tr(is_dev ? (int64_t)h : 0);
        return is_dev ? (int64_t)h : 0;   // SDL_OpenAudio returns 0 on success
    }
    if (name == "SDL_CloseAudioDevice") {
        auto it = I.sdl_devs_.find(R(0));
        if (it != I.sdl_devs_.end()) {
            it->second.stop = true;
            if (it->second.pump.joinable()) it->second.pump.join();
            if (it->second.bounce)
                I.mem->untrack_allocation(it->second.bounce, it->second.bounce_bytes);
            I.sdl_devs_.erase(it);
        }
        tr(0); return 0;
    }
    if (name == "SDL_PauseAudioDevice" || name == "SDL_PauseAudio") {
        uint64_t dev = (name[15] == 'D') ? R(0) : 1;   // SDL_PauseAudio uses dev 1
        uint8_t pause = (uint8_t)R(1);
        auto it = I.sdl_devs_.find(dev);
        if (it != I.sdl_devs_.end()) it->second.paused = pause != 0;
        tr(0); return 0;
    }
    if (name == "SDL_QueueAudio") {
        uint64_t dev = R(0), data = R(1);
        uint32_t len = (uint32_t)R(2);
        auto it = I.sdl_devs_.find(dev);
        int64_t rc = -1;
        if (it != I.sdl_devs_.end() && data && len && I.engine) {
            std::vector<uint8_t> tmp(len);
            I.mem->read(data, tmp.data(), len);
            ssize_t fr = I.engine->write_interleaved(
                it->second.fmt, it->second.rate, it->second.ch,
                tmp.data(), len);
            rc = (fr >= 0) ? 0 : -1;
        }
        tr(rc); return rc;
    }
    if (name == "SDL_GetQueuedAudioSize") {
        int64_t r = I.engine ? (int64_t)I.engine->ring_queued_bytes() : 0;
        tr(r); return r;
    }
    if (name == "SDL_ClearQueuedAudio") {
        if (I.engine) I.engine->clear_queued();
        tr(0); return 0;
    }
    if (name == "SDL_DequeueAudio" || name == "SDL_AudioInit" ||
        name == "SDL_AudioQuit" || name == "SDL_MixAudioFormat" ||
        name == "SDL_LockAudioDevice" || name == "SDL_UnlockAudioDevice") {
        tr(0); return 0;
    }
    if (name == "SDL_GetNumAudioDevices") { tr(0); return 1; }
    if (name == "SDL_GetAudioDeviceName") {
        static uint64_t namebuf = 0;
        if (!namebuf) namebuf = I.mem->mmap_alloc(64);
        const char* n = "bifrost-audio";
        I.mem->write(namebuf, n, strlen(n) + 1);
        tr((int64_t)namebuf); return (int64_t)namebuf;
    }

    // ══ ALSA (subset games actually use) ════════════════════════════
    if (name == "snd_pcm_open") {
        uint64_t pcmp = R(0);
        std::string dev = rd_cstr(I, R(1));
        if (!pcmp) { tr(-EFAULT); return -EFAULT; }
        const uint64_t h = 0xA6000000ull + I.alsa_pcms_.size() + 1;
        I.alsa_pcms_[h] = AudioThunkImpl::AlsaPcm{};
        wr64(I, pcmp, h);
        if (dbg().thunk_trace)
            fprintf(stderr, "[audio-thunk] snd_pcm_open(\"%s\") → 0x%llx\n",
                    dev.c_str(), (unsigned long long)h);
        tr(0); return 0;
    }
    if (name == "snd_pcm_hw_params_malloc") {
        uint64_t hp = R(0);
        if (!hp) { tr(-EFAULT); return -EFAULT; }
        wr64(I, hp, 0xA6100000ull + (uint64_t)(uintptr_t)&I);  // stable fake ptr
        tr(0); return 0;
    }
    if (name == "snd_pcm_hw_params_free" || name == "snd_pcm_hw_params_any" ||
        name == "snd_pcm_hw_params_set_access" || name == "snd_pcm_hw_params") {
        tr(0); return 0;
    }
    if (name == "snd_pcm_hw_params_set_format") {
        auto it = I.alsa_pcms_.find(R(0));
        if (it != I.alsa_pcms_.end()) {
            switch ((int)R(2)) {   // snd_pcm_format_t
                case 1:  it->second.fmt = PCM_FMT_U8; break;   // U8
                case 6:  it->second.fmt = PCM_FMT_S24; break;  // S24_LE
                case 14: case 15: it->second.fmt = PCM_FMT_F32; break; // FLOAT_LE/BE
                default: it->second.fmt = PCM_FMT_S16; break;  // S16_* / others
            }
        }
        tr(0); return 0;
    }
    if (name == "snd_pcm_hw_params_set_channels") {
        auto it = I.alsa_pcms_.find(R(0));
        if (it != I.alsa_pcms_.end()) it->second.ch = (uint8_t)R(2);
        tr(0); return 0;
    }
    if (name == "snd_pcm_hw_params_set_rate") {
        auto it = I.alsa_pcms_.find(R(0));
        if (it != I.alsa_pcms_.end()) it->second.rate = (uint32_t)R(2);
        uint64_t dir = R(3);
        if (dir) wr32(I, dir, 0);
        tr(0); return 0;
    }
    if (name == "snd_pcm_writei") {
        auto it = I.alsa_pcms_.find(R(0));
        int64_t rc = -EIO;
        if (it != I.alsa_pcms_.end() && I.engine) {
            uint64_t buf = R(1);
            int64_t frames = (int64_t)R(2);
            size_t fsz = (it->second.fmt == PCM_FMT_U8 ? 1 :
                          it->second.fmt == PCM_FMT_S16 ? 2 : 4);
            size_t bytes = (size_t)frames * it->second.ch * fsz;
            if (buf && bytes) {
                std::vector<uint8_t> tmp(bytes);
                I.mem->read(buf, tmp.data(), bytes);
                ssize_t fr = I.engine->write_interleaved(
                    it->second.fmt, it->second.rate, it->second.ch,
                    tmp.data(), bytes);
                rc = (fr >= 0) ? frames : -EIO;
            } else rc = frames;
        }
        tr(rc); return rc;
    }
    if (name == "snd_pcm_avail_update") { tr(4096); return 4096; }
    if (name == "snd_pcm_delay") {
        uint64_t p = R(1);
        if (p) wr64(I, p, 0);
        tr(0); return 0;
    }
    if (name == "snd_pcm_drain" || name == "snd_pcm_drop" ||
        name == "snd_pcm_pause" || name == "snd_pcm_recover" ||
        name == "snd_pcm_prepare" || name == "snd_pcm_start" ||
        name == "snd_pcm_close") {
        tr(0); return 0;
    }
    if (name == "snd_strerror") {
        static uint64_t strbuf = 0;
        if (!strbuf) strbuf = I.mem->mmap_alloc(64);
        const char* s = "audio-thunk";
        I.mem->write(strbuf, s, strlen(s) + 1);
        tr((int64_t)strbuf); return (int64_t)strbuf;
    }
    if (name == "snd_pcm_readi") { tr(-EAGAIN); return -EAGAIN; }

    // ══ PulseAudio simple API ═══════════════════════════════════════
    if (name == "pa_simple_new") {
        // (server, dev, dir, streamname, spec*, map*, attr*, error**)
        const uint64_t spec = R(4), errp = R(7);
        const uint64_t h = 0xA7000000ull + I.pulse_streams_.size() + 1;
        auto& ps = I.pulse_streams_[h];
        if (spec) {
            uint32_t pf = rd32(I, spec + 0);
            uint32_t pr = rd32(I, spec + 4);
            uint8_t  pc = 0; I.mem->read(spec + 8, &pc, 1);
            switch (pf) {
                case 0: ps.fmt = PCM_FMT_U8; break;       // PA_SAMPLE_U8
                case 5: ps.fmt = PCM_FMT_F32; break;      // PA_SAMPLE_FLOAT32LE
                default: ps.fmt = PCM_FMT_S16; break;     // S16LE/BE + others
            }
            if (pr) ps.rate = pr;
            if (pc) ps.ch = pc;
        }
        if (errp) { uint64_t z = rd64(I, errp); if (z) wr32(I, z, 0); }
        tr((int64_t)h); return (int64_t)h;
    }
    if (name == "pa_simple_write") {
        auto it = I.pulse_streams_.find(R(0));
        int64_t rc = -EIO;
        if (it != I.pulse_streams_.end() && I.engine) {
            uint64_t data = R(1);
            size_t bytes = (size_t)R(2);
            std::vector<uint8_t> tmp(bytes);
            I.mem->read(data, tmp.data(), bytes);
            ssize_t fr = I.engine->write_interleaved(
                it->second.fmt, it->second.rate, it->second.ch,
                tmp.data(), bytes);
            rc = (fr >= 0) ? 0 : -EIO;
        }
        tr(rc); return rc;
    }
    if (name == "pa_simple_drain" || name == "pa_simple_flush" ||
        name == "pa_simple_free") { tr(0); return 0; }
    if (name == "pa_simple_get_latency") { tr(0); return 0; }
    if (name == "pa_strerror") {
        static uint64_t pbuf = 0;
        if (!pbuf) pbuf = I.mem->mmap_alloc(64);
        const char* s = "audio-thunk";
        I.mem->write(pbuf, s, strlen(s) + 1);
        tr((int64_t)pbuf); return (int64_t)pbuf;
    }

    // ══ OpenAL ══════════════════════════════════════════════════════
    if (name == "alcOpenDevice") { tr(0xA8000001); return 0xA8000001; }
    if (name == "alcCloseDevice") { tr(1); return 1; }
    if (name == "alcCreateContext") {
        const uint64_t h = 0xA8100000ull + ++I.al_ctx_;
        tr((int64_t)h); return (int64_t)h;
    }
    if (name == "alcMakeContextCurrent") { tr(1); return 1; }
    if (name == "alcDestroyContext" || name == "alcProcessContext" ||
        name == "alcSuspendContext" || name == "alcGetError" ||
        name == "alGetError") { tr(0); return 0; }
    if (name == "alcGetString" || name == "alGetString") {
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
        int n = (int)R(0);
        uint64_t ids = R(1);
        for (int i = 0; i < n && ids; i++) {
            uint64_t id = 0xA9000000ull + I.al_bufs_.size() + 1;
            I.al_bufs_[id] = AudioThunkImpl::AlBuffer{};
            wr64(I, ids + (uint64_t)i * 4, (uint32_t)id);
        }
        tr(0); return 0;
    }
    if (name == "alDeleteBuffers") {
        int n = (int)R(0);
        uint64_t ids = R(1);
        for (int i = 0; i < n && ids; i++) {
            uint32_t id = rd32(I, ids + (uint64_t)i * 4);
            I.al_bufs_.erase(id);
        }
        tr(0); return 0;
    }
    if (name == "alBufferData") {
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
        int n = (int)R(0);
        uint64_t ids = R(1);
        for (int i = 0; i < n && ids; i++) {
            uint64_t id = 0xA9800000ull + I.al_srcs_.size() + 1;
            I.al_srcs_[id] = AudioThunkImpl::AlSource{};
            wr64(I, ids + (uint64_t)i * 4, (uint32_t)id);
        }
        tr(0); return 0;
    }
    if (name == "alDeleteSources") {
        int n = (int)R(0);
        uint64_t ids = R(1);
        for (int i = 0; i < n && ids; i++) I.al_srcs_.erase(rd32(I, ids + (uint64_t)i * 4));
        tr(0); return 0;
    }
    if (name == "alSourceQueueBuffers") {
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
        auto it = I.al_srcs_.find(R(0));
        if (it != I.al_srcs_.end() && I.engine) {
            auto& src = it->second;
            src.playing = true;
            for (size_t q = src.played; q < src.queue.size(); q++) {
                auto bit = I.al_bufs_.find(src.queue[q]);
                if (bit == I.al_bufs_.end()) continue;
                I.engine->write_interleaved(bit->second.fmt, bit->second.rate,
                                            bit->second.ch, bit->second.data.data(),
                                            bit->second.data.size());
            }
            src.played = src.queue.size();
        }
        tr(0); return 0;
    }
    if (name == "alSourceStop") {
        auto it = I.al_srcs_.find(R(0));
        if (it != I.al_srcs_.end()) {
            it->second.queue.clear();
            it->second.played = 0;
            it->second.playing = false;
        }
        tr(0); return 0;
    }
    if (name == "alSourceUnqueueBuffers") {
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

    // ══ AAudio (Android) ════════════════════════════════════════════
    if (name == "AAudioStreamBuilder_new") {
        uint64_t out = R(0);
        const uint64_t h = 0xAA000000ull + I.aa_builders_.size() + 1;
        I.aa_builders_[h] = AudioThunkImpl::AaBuilder{};
        if (out) wr64(I, out, h);
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_setFormat") {
        auto it = I.aa_builders_.find(R(0));
        if (it != I.aa_builders_.end()) it->second.fmt = (int32_t)R(1);
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_setChannelCount") {
        auto it = I.aa_builders_.find(R(0));
        if (it != I.aa_builders_.end()) it->second.ch = (int32_t)R(1);
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_setSampleRate") {
        auto it = I.aa_builders_.find(R(0));
        if (it != I.aa_builders_.end()) it->second.rate = (int32_t)R(1);
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_setDataCallback") {
        auto it = I.aa_builders_.find(R(0));
        if (it != I.aa_builders_.end()) { it->second.cb_fn = R(1); it->second.cb_ud = R(2); }
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_setErrorCallback") {
        auto it = I.aa_builders_.find(R(0));
        if (it != I.aa_builders_.end()) { it->second.err_fn = R(1); it->second.err_ud = R(2); }
        tr(0); return 0;
    }
    if (name == "AAudioStreamBuilder_openStream") {
        auto bit = I.aa_builders_.find(R(0));
        uint64_t out = R(1);
        if (bit == I.aa_builders_.end() || !out) { tr(-22); return -22; }
        const AudioThunkImpl::AaBuilder& b = bit->second;
        const uint64_t h = 0xAA100000ull + I.aa_streams_.size() + 1;
        auto& s = I.aa_streams_[h];
        s.fmt = (b.fmt == 2) ? PCM_FMT_F32 : PCM_FMT_S16;   // AAUDIO_FORMAT_FLOAT=2, I16=1
        s.size = (s.fmt == PCM_FMT_F32) ? 4 : 2;
        s.ch   = b.ch > 0 ? (uint8_t)b.ch : 2;
        s.rate = b.rate > 0 ? (uint32_t)b.rate : 48000;
        s.cb_fn = b.cb_fn; s.cb_ud = b.cb_ud;
        s.stream_arg = (int64_t)h;
        s.aaudio = true;
        wr64(I, out, h);
        if (s.cb_fn) {
            s.bounce_bytes = (size_t)s.frames_per_cb * s.ch * s.size;
            s.bounce = I.mem->mmap_alloc(s.bounce_bytes);
            if (!s.bounce) { I.aa_streams_.erase(h); tr(-ENOMEM); return -ENOMEM; }
            s.pump = std::thread([&I, h, pump_loop] {
                auto it = I.aa_streams_.find(h);
                if (it == I.aa_streams_.end()) return;
                pump_loop(it->second);
            });
        }
        I.engine->open(s.rate, s.ch, s.size);
        tr(0); return 0;   // AAUDIO_OK
    }
    if (name == "AAudioStream_requestStart") {
        auto it = I.aa_streams_.find(R(0));
        if (it != I.aa_streams_.end()) it->second.paused = false;
        tr(0); return 0;
    }
    if (name == "AAudioStream_requestPause" || name == "AAudioStream_requestStop") {
        auto it = I.aa_streams_.find(R(0));
        if (it != I.aa_streams_.end()) it->second.paused = true;
        tr(0); return 0;
    }
    if (name == "AAudioStream_close") {
        auto it = I.aa_streams_.find(R(0));
        if (it != I.aa_streams_.end()) {
            it->second.stop = true;
            if (it->second.pump.joinable()) it->second.pump.join();
            if (it->second.bounce)
                I.mem->untrack_allocation(it->second.bounce, it->second.bounce_bytes);
            I.aa_streams_.erase(it);
        }
        tr(0); return 0;
    }
    if (name == "AAudioStream_write") {
        auto it = I.aa_streams_.find(R(0));
        int64_t rc = -EIO;
        if (it != I.aa_streams_.end() && I.engine) {
            uint64_t data = R(1);
            int32_t frames = (int32_t)R(2);
            size_t bytes = (size_t)frames * it->second.ch * it->second.size;
            std::vector<uint8_t> tmp(bytes);
            I.mem->read(data, tmp.data(), bytes);
            ssize_t fr = I.engine->write_interleaved(
                it->second.fmt, it->second.rate, it->second.ch,
                tmp.data(), bytes);
            rc = (fr >= 0) ? frames : -EIO;
        }
        tr(rc); return rc;
    }
    if (name == "AAudioStream_getState") {
        auto it = I.aa_streams_.find(R(0));
        int64_t st = (it != I.aa_streams_.end() && !it->second.paused &&
                      !it->second.stop.load()) ? 4 /*Started*/ : 10 /*Stopped*/;
        tr(st); return st;
    }
    if (name == "AAudioStream_waitForStateChange") { tr(0); return 0; }
    if (name == "AAudioStream_getFormat") {
        auto it = I.aa_streams_.find(R(0));
        int64_t f = (it != I.aa_streams_.end() && it->second.fmt == PCM_FMT_F32) ? 2 : 1;
        tr(f); return f;
    }
    if (name == "AAudioStream_getChannelCount") {
        auto it = I.aa_streams_.find(R(0));
        int64_t c = it != I.aa_streams_.end() ? it->second.ch : 2;
        tr(c); return c;
    }
    if (name == "AAudioStream_getSampleRate") {
        auto it = I.aa_streams_.find(R(0));
        int64_t r = it != I.aa_streams_.end() ? (int64_t)it->second.rate : 48000;
        tr(r); return r;
    }
    if (name == "AAudioStream_getBufferSizeInFrames" ||
        name == "AAudioStream_getFramesPerBurst" ||
        name == "AAudioStream_getFramesPerDataCallback") {
        auto it = I.aa_streams_.find(R(0));
        int64_t r = it != I.aa_streams_.end() ? it->second.frames_per_cb : 1024;
        tr(r); return r;
    }
    if (name == "AAudioStream_getFramesRead" || name == "AAudioStream_getFramesWritten") {
        int64_t r = I.engine ? (int64_t)(I.engine->bytes_pushed() /
                    (4 * 2)) : 0;   // rough frame count at S16 stereo
        tr(r); return r;
    }

    // ══ OpenSL ES (synthetic vtables) ═══════════════════════════════
    // Object model: SLObjectItf is a pointer W where *W is a vtable of
    // function pointers into our trampolines. IID blobs resolve via the
    // SL_IID_* symbol rows (pointer equality against their trampoline
    // addresses — guests never dereference them).
    auto make_vtable = [this, &I](const std::vector<std::string>& syms,
                            uint64_t* word_out) -> bool {
        // allocate vtable block + itf-pointer word in guest RAM
        uint64_t blk = I.mem->mmap_alloc(4096);
        if (!blk) return false;
        uint64_t vt = blk + 64;                       // word lives at blk+0..7
        wr64(I, blk, vt);
        for (size_t i = 0; i < syms.size(); i++) {
            uint64_t addr = resolve("libOpenSLES.so", syms[i]);
            wr64(I, vt + i * 8, addr);
        }
        *word_out = blk;
        return true;
    };
    if (name == "slCreateEngine") {
        uint64_t out = R(0);
        if (!out) { tr(-22); return -22; }
        auto obj = std::make_unique<AudioThunkImpl::SlObject>();
        obj->kind = 0;
        if (!make_vtable({"__osl_realize", "__osl_destroy", "__osl_getinterface"},
                         &obj->itf_word)) { tr(-ENOMEM); return -ENOMEM; }
        if (!make_vtable({"__osl_eng_createmix", "__osl_eng_createplayer"},
                         &obj->play_itf_word)) obj->play_itf_word = 0;  // reuse as eng-itf word
        AudioThunkImpl::SlObject* raw = obj.get();
        I.sl_objs_.push_back(std::move(obj));
        I.sl_by_word_[raw->itf_word] = raw;
        I.sl_eng_by_word_[raw->play_itf_word] = raw;    // engine interface lookup
        wr64(I, out, raw->itf_word);
        tr(0); return 0;   // SL_RESULT_SUCCESS
    }
    if (name == "__osl_realize") {
        auto it = I.sl_by_word_.find(R(0));
        if (it != I.sl_by_word_.end()) it->second->realized = true;
        tr(0); return 0;
    }
    if (name == "__osl_destroy") {
        auto it = I.sl_by_word_.find(R(0));
        if (it != I.sl_by_word_.end()) {
            it->second->realized = false;
            I.sl_bq_by_word_.erase(it->second->bq_itf_word);
            I.sl_play_by_word_.erase(it->second->play_itf_word);
        }
        tr(0); return 0;
    }
    if (name == "__osl_getinterface") {
        // (itf_word, SLInterfaceID iid*, void** out)
        auto it = I.sl_by_word_.find(R(0));
        uint64_t iid = R(1), outp = R(2);
        if (it == I.sl_by_word_.end() || !outp || !iid) { tr(1); return 1; }
        auto tag = I.sl_iids_.find(iid);
        uint64_t t = (tag != I.sl_iids_.end()) ? tag->second : (uint64_t)-1;
        AudioThunkImpl::SlObject& o = *it->second;
        switch (t) {
            case 0:   // SL_IID_ENGINE → engine-interface vtable
                wr64(I, outp, o.play_itf_word);
                break;
            case 1:   // SL_IID_OUTPUTMIX → object itself
                wr64(I, outp, o.itf_word);
                break;
            case 2: { // SL_IID_BUFFERQUEUE → per-object bq itf
                if (!o.bq_itf_word) {
                    if (!make_vtable({"__osl_bq_enqueue", "__osl_bq_register"},
                                     &o.bq_itf_word)) { tr(1); return 1; }
                    I.sl_bq_by_word_[o.bq_itf_word] = &o;
                }
                wr64(I, outp, o.bq_itf_word);
                break;
            }
            case 3: { // SL_IID_PLAY → per-object play itf
                if (!o.play_itf_word || !I.sl_play_by_word_.count(o.play_itf_word)) {
                    uint64_t w = 0;
                    if (!make_vtable({"__osl_play_setstate"}, &w)) { tr(1); return 1; }
                    o.play_itf_word = w;
                    I.sl_play_by_word_[w] = &o;
                }
                wr64(I, outp, o.play_itf_word);
                break;
            }
            default:
                wr64(I, outp, o.itf_word);
                break;
        }
        tr(0); return 0;
    }
    if (name == "__osl_eng_createmix") {
        uint64_t out = R(1);
        if (!out) { tr(1); return 1; }
        auto obj = std::make_unique<AudioThunkImpl::SlObject>();
        obj->kind = 1;
        if (!make_vtable({"__osl_realize", "__osl_destroy", "__osl_getinterface"},
                         &obj->itf_word)) { tr(1); return 1; }
        AudioThunkImpl::SlObject* raw = obj.get();
        I.sl_objs_.push_back(std::move(obj));
        I.sl_by_word_[raw->itf_word] = raw;
        wr64(I, out, raw->itf_word);
        tr(0); return 0;
    }
    if (name == "__osl_eng_createplayer") {
        uint64_t out = R(1);
        if (!out) { tr(1); return 1; }
        auto obj = std::make_unique<AudioThunkImpl::SlObject>();
        obj->kind = 2;
        if (!make_vtable({"__osl_realize", "__osl_destroy", "__osl_getinterface"},
                         &obj->itf_word)) { tr(1); return 1; }
        AudioThunkImpl::SlObject* raw = obj.get();
        I.sl_objs_.push_back(std::move(obj));
        I.sl_by_word_[raw->itf_word] = raw;
        wr64(I, out, raw->itf_word);
        tr(0); return 0;
    }
    if (name == "__osl_bq_enqueue") {
        auto it = I.sl_bq_by_word_.find(R(0));
        int64_t rc = 1;   // SL_RESULT_PARAMETER_INVALID
        if (it != I.sl_bq_by_word_.end() && I.engine) {
            uint64_t data = R(1);
            size_t bytes = (size_t)R(2);
            std::vector<uint8_t> tmp(bytes);
            I.mem->read(data, tmp.data(), bytes);
            // Players configure format via SLDataSource; default S16/44100/2
            I.engine->write_interleaved(PCM_FMT_S16, 44100, 2, tmp.data(), bytes);
            rc = 0;   // SL_RESULT_SUCCESS
            // Fire the registered callback INLINE (guest thread): mirrors
            // BufferQueue semantics closely enough for streaming players.
            AudioThunkImpl::SlObject* o = it->second;
            if (o->bq_cb_fn && I.runner && I.cb_cpu) {
                int64_t ia[2] = { (int64_t)R(0), (int64_t)o->bq_cb_ctx };
                I.runner(*I.cb_cpu, o->bq_cb_fn, ia, 2);
            }
        }
        tr(rc); return rc;
    }
    if (name == "__osl_bq_register") {
        auto it = I.sl_bq_by_word_.find(R(0));
        if (it != I.sl_bq_by_word_.end()) {
            it->second->bq_cb_fn = R(1);
            it->second->bq_cb_ctx = R(2);
        }
        tr(0); return 0;
    }
    if (name == "__osl_play_setstate") { tr(0); return 0; }

    // ══ fallback: generic host-fn dispatch (legacy rows) ════════════
    return thunk_dispatch_generic(cpu, entry.host_fn, entry.name, trace);
}// ── register_known_symbols_ ────────────────────────────────────────────
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
    REG(aa_libs, AAudioStreamBuilder_delete);
    REG(aa_libs, AAudioStreamBuilder_setFormat);
    REG(aa_libs, AAudioStreamBuilder_setChannelCount);
    REG(aa_libs, AAudioStreamBuilder_setSampleRate);
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
} // namespace arm64emu
