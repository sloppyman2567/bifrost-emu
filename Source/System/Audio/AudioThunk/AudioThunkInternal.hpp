// AudioThunkInternal.hpp — AudioThunk private state and the per-call dispatch
// context shared by the files in Source/System/Audio/AudioThunk/.
#pragma once
#include "System/Audio/AudioThunk.hpp"
#include "System/Audio/Audio.hpp"
#include "System/Graphics/Thunks/GraphicThunk.hpp"  // for SYSCALL_NUMBER constant
#include "System/Graphics/Thunks/ThunkCommon.hpp"
#include "System/Core/DebugFlags.hpp"    // dbg() — cached trace gates (BIFROST_THUNK_TRACE)
#include "System/Core/CPU.hpp"
#include "System/Memory/Memory.hpp"
#include <dlfcn.h>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <new>
#include <unistd.h>   // usleep (blocking-write backoff)
#include <optional>
#include <string>
#include <vector>
namespace arm64emu {
struct AudioThunkImpl {
    bool   enabled = false;
    bool   initialized = false;
    Memory* mem    = nullptr;
    Audio*  engine = nullptr;          // borrowed (owned by Emulator)
    AudioCbRunner runner;              // may be empty → callbacks dropped
    AudioCbCpuFactory cpu_factory;
    uint64_t trampoline_base = 0;
    static constexpr uint64_t TRAMPOLINE_PAGE_SIZE =
        AudioThunk::TRAMPOLINE_SIZE * AudioThunk::MAX_SYMBOLS;  // 16 KiB
    std::vector<ThunkLibTable> libs_;
    std::vector<std::pair<uint32_t, uint32_t>> id_to_idx_;
    // Recursive: dispatch arms hold this across map access while helpers
    // they call (resolve via make_vtable, runner-reentrant dispatches on
    // the same thread) lock it again.
    std::recursive_mutex mu;
    std::unique_ptr<CPU> pump_cpu;
    std::thread pump_thread;
    std::atomic<bool> pump_go{false};
    std::atomic<bool> pump_enabled{false};
    std::condition_variable callback_cv;
    std::mutex pump_mu;             // guards stream maps and callback state
    std::mutex pump_wait_mu;
    std::condition_variable pump_cv;
    std::atomic<uint64_t> pump_wake_generation{0};

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
        int32_t aa_state = 2;                // AAUDIO_STREAM_STATE_OPEN
        int32_t aa_session = -1;
        int32_t aa_usage = 1, aa_input_preset = 6;
        uint32_t aa_buffer_frames = 0;
        // AAudio: a guest thread made by the Android libaaudio adapter runs
        // the data callback, so the pump and inline firing skip the stream.
        bool guest_thread = false;
        std::atomic<bool> stop{false};
        std::atomic<bool> paused{false};
        // ── SDL2 queue-mode backlog (real-SDL semantics) ──────────────
        // Bytes queued via SDL_QueueAudio that have not yet been handed
        // to the engine ring. Real SDL buffers ALL queued bytes and
        // plays them back-to-back; buffering here (instead of dropping
        // on a full ring) keeps music gap-free, makes
        // SDL_GetQueuedAudioSize report the device's true backlog, and
        // lets SDL_PauseAudioDevice actually gate playback. Drained to
        // the ring lazily by top_up_queue_() from audio-thunk dispatches
        // on the guest thread when pump mode is disabled.
        std::vector<uint8_t> pending;
        size_t pending_off = 0;
        // Logical Audio-engine stream (per-device float ring + mixer).
        // 0 = not created yet (lazily opened by arms that know fmt/rate).
        int engine_stream = 0;
        // Deferred guest-callback schedule (callback-mode devices).
        uint64_t next_cb_us = 0;      // steady-clock µs of next fire
        bool cb_scheduled = false;
        uint32_t callback_lock_depth = 0;  // SDL_LockAudioDevice nesting
        bool callback_active = false;
        bool close_deferred = false;
        std::thread::id callback_host{};
    };
    std::map<uint64_t, PumpStream> sdl_devs_;
    std::map<uint64_t, PumpStream> aa_streams_;
    // Monotonic handle generators. Minting from map size() collides after
    // any close/reopen cycle (open A, open B, close A, open C ⇒ C aliases
    // B's live slot via map::operator[]). SDL devices use real-SDL-style
    // ids starting at 1 — the legacy SDL_PauseAudio/SDL_CloseAudio arms
    // address the global device as literal 1.
    // SDL reserves device ID 1 for the legacy SDL_OpenAudio API.
    uint64_t next_sdl_dev_ = 1;
    uint64_t next_alsa_pcm_ = 0;
    uint64_t next_pulse_ = 0;
    uint64_t next_al_buf_ = 0;
    uint64_t next_al_src_ = 0;
    uint64_t next_aa_builder_ = 0;
    uint64_t next_aa_stream_ = 0;

    // ── ALSA pcm handles ──────────────────────────────────────────────
    struct AlsaPcm { uint32_t fmt = PCM_FMT_S16; uint32_t rate = 44100; uint8_t ch = 2;
                     int engine_stream = 0; };
    std::map<uint64_t, AlsaPcm> alsa_pcms_;

    // ── Pulse simple streams ──────────────────────────────────────────
    struct PulseSimple { uint32_t fmt = PCM_FMT_S16; uint32_t rate = 44100; uint8_t ch = 2;
                         int engine_stream = 0; };
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
        int engine_stream = 0;             // lazily created on first play
    };
    std::map<uint64_t, AlBuffer> al_bufs_;
    std::map<uint64_t, AlSource> al_srcs_;
    uint64_t al_ctx_ = 0;

    // ── AAudio builders ───────────────────────────────────────────────
    struct AaBuilder {
        int32_t fmt = 0;                   // AAUDIO_FORMAT_UNSPECIFIED=0/I16=1/FLOAT=2
        int32_t ch = 0, rate = 0, frames_per_cb = 0;
        int32_t device = 0, direction = 0, sharing = 1, performance = 10;
        int32_t session = -1, capacity = 0;
        int32_t usage = 1, input_preset = 6;
        uint64_t cb_fn = 0, cb_ud = 0, err_fn = 0, err_ud = 0;
    };
    std::map<uint64_t, AaBuilder> aa_builders_;

    void wake_pump() {
        // Serialize the predicate change with wait_for's unlock-and-wait.
        // An atomic generation alone does not prevent notify_one landing
        // after the predicate read but before the worker actually sleeps.
        // Callers may hold pump_mu; the waiter never takes it while holding
        // pump_wait_mu, so the lock order is always pump_mu -> pump_wait_mu.
        {
            std::lock_guard<std::mutex> lock(pump_wait_mu);
            pump_wake_generation.fetch_add(1, std::memory_order_release);
        }
        pump_cv.notify_one();
    }

    // ── OpenSL ES objects/interfaces (synthetic vtables in guest RAM) ─
    struct SlObject {
        uint64_t itf_word = 0;             // address of the itf-pointer word
        uint64_t kind = 0;                 // 0=engine,1=outputmix,2=player
        bool realized = false;
        int engine_stream = 0;             // players: per-source mixer stream
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
        {
            std::lock_guard<std::mutex> lock(pump_wait_mu);
            pump_go = false;
        }
        pump_cv.notify_all();
        if (pump_thread.joinable()) pump_thread.join();
        pump_enabled = false;
        pump_cpu.reset();
        for (auto& [h, d] : sdl_devs_) {
            d.stop = true;
        }
        for (auto& [h, s] : aa_streams_) {
            s.stop = true;
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
inline std::string rd_cstr(AudioThunkImpl& I, uint64_t a) {
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

// Reentrancy guard for inline guest audio callbacks (DispatchHelpers.cpp).
inline thread_local bool audio_in_callbacks = false;

// One dispatch() call: the resolved symbol plus the helpers its API
// stages share. Each stage returns a value when it handled the symbol.
struct AudioDispatch {
    AudioThunk& self;
    AudioThunkImpl& I;
    CPU& cpu;
    const ThunkSymbolEntry& entry;
    const std::string& name;
    const bool trace;
    void tr(int64_t r) const;
    static void fill_cb_args(const AudioThunkImpl::PumpStream& s, int64_t* ia, size_t& na,
                             size_t cb_bytes);
    static uint32_t sdl_fmt_map(uint16_t f);
    static uint8_t fmt_size(uint32_t f);
    size_t stream_write_wait(int estream, uint32_t fmt, uint32_t rate, uint8_t ch,
                             const uint8_t* data, size_t bytes);
    void top_up_queue(AudioThunkImpl::PumpStream& s);
    void run_due_callbacks();
    uint64_t resolve(const std::string& lib, const std::string& sym) {
        return self.resolve(lib, sym);
    }
    void start_pump() { self.start_pump(); }
    std::optional<int64_t> sdl();
    std::optional<int64_t> alsa();
    std::optional<int64_t> pulse();
    std::optional<int64_t> openal();
    std::optional<int64_t> aaudio();
    std::optional<int64_t> opensl();
};
}  // namespace arm64emu
