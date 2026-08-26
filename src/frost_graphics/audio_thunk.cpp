// frost_graphics/audio_thunk.cpp — AudioThunk implementation (1.5.5-alpha).
//
// See include/frost/audio_thunk.hpp for the design overview.
//
// 1.5.5-alpha REWRITE ("working audio path"): guest audio API calls are NO
// LONGER forwarded to same-name host libraries (host libasound/libpulse
// deref opaque structs expecting HOST pointers — a dead end that forced the
// old build to stub everything). Instead every arm converts its call into
// plain sample pushes on the shared AudioEngine ring via
// Audio::stream_write() — one mixer,
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
#include "core/cpu.h"
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
    // ── dedicated-vCPU audio pump ─────────────────────────────────────
    // A host clock thread fires guest data callbacks on an EXCLUSIVE
    // cloned vCPU (pump_cpu) at device cadence — real SMP semantics,
    // mirroring Android's in-process AAudio callback thread. Never
    // touches cb_cpu (the main vCPU), so it cannot corrupt running
    // guest state the way the old borrow-main-CPU pump did.
    std::unique_ptr<CPU> pump_cpu;
    std::thread pump_thread;
    std::atomic<bool> pump_go{false};
    bool pump_enabled = false;      // set by start_pump()
    std::recursive_mutex pump_mu;   // guards map structure vs pump passes
                                    // (recursive: callbacks may close their
                                    //  own device mid-fire)

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
        // ── SDL2 queue-mode backlog (real-SDL semantics) ──────────────
        // Bytes queued via SDL_QueueAudio that have not yet been handed
        // to the engine ring. Real SDL buffers ALL queued bytes and
        // plays them back-to-back; buffering here (instead of dropping
        // on a full ring) keeps music gap-free, makes
        // SDL_GetQueuedAudioSize report the device's true backlog, and
        // lets SDL_PauseAudioDevice actually gate playback. Drained to
        // the ring lazily by top_up_queue_() from audio-thunk dispatches
        // on the guest thread — no pump threads, no async guest calls.
        std::vector<uint8_t> pending;
        size_t pending_off = 0;
        // Logical Audio-engine stream (per-device float ring + mixer).
        // 0 = not created yet (lazily opened by arms that know fmt/rate).
        int engine_stream = 0;
        // Deferred guest-callback schedule (callback-mode devices). The
        // callback fires INLINE from audio-thunk dispatches on the guest
        // thread when due — never from a host pump thread.
        uint64_t next_cb_us = 0;      // steady-clock µs of next fire
        bool cb_scheduled = false;
    };
    std::map<uint64_t, PumpStream> sdl_devs_;
    std::map<uint64_t, PumpStream> aa_streams_;
    // Monotonic handle generators. Minting from map size() collides after
    // any close/reopen cycle (open A, open B, close A, open C ⇒ C aliases
    // B's live slot via map::operator[]). SDL devices use real-SDL-style
    // ids starting at 1 — the legacy SDL_PauseAudio/SDL_CloseAudio arms
    // address the global device as literal 1.
    uint64_t next_sdl_dev_ = 0;
    // Guest VA of __libc_single_threaded (set by Emulator::wire_...);
    // cleared when the pump starts so glibc takes malloc locks while the
    // pump vCPU fires callbacks concurrently with the main thread.
    uint64_t libc_st_addr_ = 0;
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
        int32_t ch = 0, rate = 0;
        uint64_t cb_fn = 0, cb_ud = 0, err_fn = 0, err_ud = 0;
    };
    std::map<uint64_t, AaBuilder> aa_builders_;

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
        pump_go = false;
        if (pump_thread.joinable()) pump_thread.join();
        pump_cpu.reset();
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
// ── dedicated-vCPU audio pump ──────────────────────────────────────────
// One host clock thread; guest data callbacks execute on an EXCLUSIVE
// cloned vCPU (never the main one), so callback timing is decoupled from
// the guest's API-call cadence — the property AAA Android engines depend
// on. Concurrency with the main guest thread is genuine SMP: the game is
// responsible for synchronizing its shared state, exactly as it would be
// on a real device.
void AudioThunk::set_libc_single_threaded_addr(uint64_t addr) {
    if (impl_) impl_->libc_st_addr_ = addr;
}

void AudioThunk::start_pump() {
    if (!impl_ || !impl_->enabled || !impl_->initialized) return;
    if (!impl_->runner || !impl_->cb_cpu || !impl_->engine) return;
    if (impl_->pump_enabled) return;
    static const bool off = [] {
        const char* e = getenv("BIFROST_AUDIO_PUMP");
        return e && e[0] == '0';
    }();
    if (off) return;
    // Arch-state snapshot of the main CPU: carries TPIDR_EL0 (TLS), FPCR,
    // etc. Registers/PC/SP are scratch — call_guest_function sets them per
    // invocation and saves/restores everything around each call. CPU is
    // non-copyable (page cache + atomics), so copy the architectural
    // fields explicitly; the rest starts fresh.
    {
        impl_->pump_cpu = std::make_unique<CPU>();
        CPU& d = *impl_->pump_cpu;
        const CPU& s = *impl_->cb_cpu;
        std::memcpy(d.regs, s.regs, sizeof d.regs);
        d.sp = s.sp; d.pc = s.pc; d.pstate = s.pstate;
        std::memcpy(d.v_lo, s.v_lo, sizeof d.v_lo);
        std::memcpy(d.v_hi, s.v_hi, sizeof d.v_hi);
        d.fpcr = s.fpcr; d.fpsr = s.fpsr;
        d.tpidr_el0 = s.tpidr_el0;
        d.tpidrro_el0 = s.tpidrro_el0;
        d.tid = -1;   // diagnostic marker: emulator-owned vCPU
    }
    impl_->pump_enabled = true;
    impl_->pump_go = true;
    // The pump fires guest callbacks on a SECOND vCPU. glibc must treat
    // the process as multi-threaded from now on or malloc runs LOCK-FREE
    // in both threads — the callback's alloc/free races the main
    // thread's and shreds the heap (neverball "malloc(): invalid size",
    // ~8s in; vkQuake AllocBlock shredding). Mirrors what
    // spawn_sdl_thread does for SDL-created guest threads.
    if (impl_->libc_st_addr_) {
        impl_->mem->store<uint32_t>(impl_->libc_st_addr_, 0);
        if (dbg().thunk_trace)
            fprintf(stderr, "[audio] pump: cleared __libc_single_threaded @0x%llx\n",
                    (unsigned long long)impl_->libc_st_addr_);
    }
    auto* impl = impl_.get();
    impl->pump_thread = std::thread([impl]() {
        AudioThunkImpl& I = *impl;
        auto now_us_fn = [] {
            return (uint64_t)std::chrono::duration_cast<
                std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        while (I.pump_go.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            const uint64_t now_us = now_us_fn();
            // Snapshot ONE due callback under the pump lock (bounce bytes
            // are read here too — worst case they hold last period's data
            // on the very first fire, matching real-device warmup).
            uint64_t fn = 0, h = 0;
            int64_t ia[4]; size_t na = 0;
            std::vector<uint8_t> buf;
            bool aaudio = false;
            int engine_stream = 0;
            uint32_t fmt = PCM_FMT_S16; uint32_t rate = 44100; uint8_t ch = 2;
            {
                std::lock_guard<std::recursive_mutex> g(I.pump_mu);
                auto scan = [&](auto& devs) -> bool {
                    for (auto& [hh, s] : devs) {
                        if (!s.cb_fn || !s.bounce || !s.cb_scheduled) continue;
                        if (s.paused.load(std::memory_order_relaxed)) {
                            s.next_cb_us = now_us;   // resume on unpause
                            continue;
                        }
                        const size_t cb_bytes =
                            (size_t)s.frames_per_cb * s.ch * s.size;
                        if (!cb_bytes) continue;
                        uint64_t due = s.next_cb_us;
                        if (now_us < due) continue;
                        if (now_us - due > 500000ULL) {  // long stall: resync
                            s.next_cb_us = now_us;
                            continue;
                        }
                        fn = s.cb_fn; h = hh; aaudio = s.aaudio;
                        fmt = s.fmt; rate = s.rate; ch = s.ch;
                        engine_stream = s.engine_stream;
                        if (s.aaudio) {
                            ia[0] = (int64_t)s.stream_arg;
                            ia[1] = (int64_t)s.cb_ud;
                            ia[2] = (int64_t)s.bounce;
                            ia[3] = (int64_t)s.frames_per_cb;
                            na = 4;
                        } else {
                            ia[0] = (int64_t)s.cb_ud;
                            ia[1] = (int64_t)s.bounce;
                            ia[2] = (int64_t)cb_bytes;
                            na = 3;
                        }
                        buf.resize(cb_bytes);
                        try {
                            I.mem->read(s.bounce, buf.data(), cb_bytes);
                        } catch (...) { buf.assign(cb_bytes, 0); }
                        const uint64_t period_us = std::max<uint64_t>(
                            (uint64_t)cb_bytes * 1000000ULL /
                                ((uint64_t)s.rate * s.ch * s.size),
                            1000);
                        s.next_cb_us = now_us + period_us;
                        return true;
                    }
                    return false;
                };
                if (scan(I.sdl_devs_)) { /* found */ }
                else scan(I.aa_streams_);
            }
            if (!fn || !h) continue;
            // Fire OUTSIDE any lock. This runs on the pump vCPU only;
            // the guest's own threads are untouched by the mechanics.
            I.runner(*I.pump_cpu, fn, ia, na);
            // The callback may have closed its own device — re-check
            // before pushing samples into its stream.
            std::lock_guard<std::recursive_mutex> g(I.pump_mu);
            if (buf.empty() || !engine_stream || !I.engine) continue;
            bool alive = false;
            auto still = [&](auto& devs) {
                auto it = devs.find(h);
                alive = it != devs.end();
            };
            still(I.sdl_devs_);
            if (!alive) still(I.aa_streams_);
            if (alive)
                I.engine->stream_write(engine_stream, fmt, rate, ch,
                                       buf.data(), buf.size());
        }
    });
    if (dbg().thunk_trace) {
        fprintf(stderr, "[audio-thunk] dedicated-vCPU pump started "
                "(BIFROST_AUDIO_PUMP=0 to disable)\n");
    }
}
// ── fork-child handling ────────────────────────────────────────────────
// Host ::fork() keeps only the calling thread. The child inherits the
// pump's std::thread OBJECT (joinable!) whose real thread lives only in
// the parent — joining it at shutdown would futex-wait forever (this
// hung every forked guest at exit). Detach the phantom and give the
// child a fresh pump of its own.
void AudioThunk::detach_pump_for_fork_child() {
    if (!impl_) return;
    impl_->pump_go = false;
    if (impl_->pump_thread.joinable()) impl_->pump_thread.detach();
    impl_->pump_enabled = false;
    impl_->pump_cpu.reset();
    start_pump();   // fresh thread + vCPU for the child
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


static thread_local bool audio_in_callbacks = false;
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

    // Shared callback-arg builder: SDL2 callbacks are (userdata, stream,
    // len); AAudio data callbacks are (stream, userdata, audioData, frames).
    auto fill_cb_args = [](const AudioThunkImpl::PumpStream& s,
                           int64_t* ia, size_t& na, size_t cb_bytes) {
        if (s.aaudio) {
            ia[0] = (int64_t)s.stream_arg;
            ia[1] = (int64_t)s.cb_ud;
            ia[2] = (int64_t)s.bounce;
            ia[3] = (int64_t)s.frames_per_cb;
            na = 4;
        } else {  // SDL2: (userdata, stream, len)
            ia[0] = (int64_t)s.cb_ud;
            ia[1] = (int64_t)s.bounce;
            ia[2] = (int64_t)cb_bytes;
            na = 3;
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
    // Drain a device's queue-mode backlog into ITS OWN engine stream
    // (real-SDL semantics: each logical device has an independent queue;
    // the engine mixer sums all devices). Backpressure is per-stream —
    // a full stream ring makes stream_write accept 0 and we retry on the
    // next dispatch. No global level check: one chatty device can never
    // starve another (the old shared-ring design let music permanently
    // pin the ring level and silently kill every other device).
    // Called from audio-thunk dispatches on the guest thread — no pump
    // threads, no async guest callbacks.
    auto top_up_queue = [&I](AudioThunkImpl::PumpStream& s) {
        if (!I.engine || !s.engine_stream) return;
        constexpr size_t kChunk = 16 * 1024;
        const size_t frame_sz =
            (size_t)(s.fmt == PCM_FMT_U8 ? 1 : (s.fmt == PCM_FMT_F32 ? 4 : 2)) * s.ch;
        while (!s.paused.load(std::memory_order_relaxed) &&
               s.pending_off < s.pending.size()) {
            size_t n = std::min(kChunk, s.pending.size() - s.pending_off);
            ssize_t fr = I.engine->stream_write(
                s.engine_stream, s.fmt, s.rate, s.ch,
                s.pending.data() + s.pending_off, n);
            if (fr <= 0) break;   // stream ring full — retry next dispatch
            s.pending_off += (size_t)fr * frame_sz;
        }
        // Compact once the consumed prefix gets large; reset when empty.
        if (s.pending_off >= s.pending.size()) {
            s.pending.clear();
            s.pending_off = 0;
        } else if (s.pending_off > 1024 * 1024) {
            s.pending.erase(s.pending.begin(),
                            s.pending.begin() + (long)s.pending_off);
            s.pending_off = 0;
        }
    };
    // Fire due guest audio callbacks INLINE on the guest thread. This is
    // the safe replacement for the banned pump threads: the borrow-CPU
    // runner runs from inside the syscall path of a SUSPENDED guest (the
    // same proven context as dlopen's guest_call_args_), never
    // concurrently with executing guest code. Called from frequently-hit
    // SDL audio arms; a reentrancy guard covers guest callbacks that
    // themselves call back into SDL.
    auto run_due_callbacks = [&I, &fill_cb_args]() {
        if (I.pump_enabled) return;   // dedicated-vCPU pump owns callbacks
        if (audio_in_callbacks || !I.runner || !I.cb_cpu || !I.engine) return;
        audio_in_callbacks = true;
        struct Reset { ~Reset() { audio_in_callbacks = false; } } reset_guard;
        const uint64_t now_us = [] {
            return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }();
        // Snapshot device handles: a guest callback may itself call
        // SDL_CloseAudioDevice / AAudioStream_close etc., mutating the
        // maps mid-iteration.
        auto pump_due = [&I, &now_us, &fill_cb_args](
                            std::map<uint64_t, AudioThunkImpl::PumpStream>&
                                devs,
                            uint64_t h) {
            auto dit = devs.find(h);
            if (dit == devs.end()) return;
            AudioThunkImpl::PumpStream* sp = &dit->second;
            AudioThunkImpl::PumpStream& s = *sp;   // node-stable until erase
            if (!s.cb_fn || !s.bounce || !s.cb_scheduled) return;
            if (s.paused.load(std::memory_order_relaxed)) {
                s.next_cb_us = now_us;   // resume on unpause, no burst
                return;
            }
            const size_t cb_bytes =
                (size_t)s.frames_per_cb * s.ch * s.size;
            if (!cb_bytes) return;
            const uint64_t period_us = std::max<uint64_t>(
                (uint64_t)cb_bytes * 1000000ULL /
                    ((uint64_t)s.rate * s.ch * s.size),
                1000);
            int fired = 0;
            while (now_us >= s.next_cb_us && fired++ < 4) {
                if (now_us - s.next_cb_us > 500000ULL) {
                    // Fell far behind (long stall) — resync, don't burst.
                    s.next_cb_us = now_us;
                    break;
                }
                int64_t ia[4];
                size_t na;
                fill_cb_args(s, ia, na, cb_bytes);
                const uint64_t cb_fn = s.cb_fn;
                I.runner(*I.cb_cpu, cb_fn, ia, na);
                // The callback may have closed its own device — the map
                // node (and s) is gone; stop touching it.
                dit = devs.find(h);
                if (dit == devs.end()) return;
                sp = &dit->second;   // rebind (same node unless erased)
                try {
                    std::vector<uint8_t> tmp(cb_bytes);
                    I.mem->read(s.bounce, tmp.data(), cb_bytes);
                    // Push into THIS device's own stream — the engine
                    // mixer sums all streams, so callback audio layers
                    // over other devices the way real SDL mixes.
                    if (s.engine_stream)
                        I.engine->stream_write(s.engine_stream, s.fmt,
                                               s.rate, s.ch, tmp.data(),
                                               cb_bytes);
                } catch (...) { /* unmapped bounce — skip period */ }
                s.next_cb_us += period_us;
            }
        };
        std::vector<uint64_t> handles;
        handles.reserve(I.sdl_devs_.size());
        for (auto& [h, s] : I.sdl_devs_) { (void)s; handles.push_back(h); }
        for (uint64_t h : handles)
            pump_due(I.sdl_devs_, h);   // re-finds; no-op if closed meanwhile
        handles.clear();
        handles.reserve(I.aa_streams_.size());
        for (auto& [h, s] : I.aa_streams_) { (void)s; handles.push_back(h); }
        for (uint64_t h : handles)
            pump_due(I.aa_streams_, h);
    };

    // ══ SDL2 ════════════════════════════════════════════════════════
    // Guest SDL_AudioSpec layout (AAPCS64): freq@0 i32, format@4 u16,
    // channels@6 u8, silence@7 u8, samples@8 u16, padding@10 u16,
    // size@12 u32, pad@14, callback@16 ptr, userdata@24 ptr. Size 32.
    if (name == "SDL_OpenAudioDevice" || name == "SDL_OpenAudio") {
        const bool is_dev = (name == "SDL_OpenAudioDevice");
        const uint64_t desired  = is_dev ? R(2) : R(0);
        const uint64_t obtained = is_dev ? R(3) : R(1);
        // BIFROST_AUDIO_FAIL_OPEN=1: report "no device" (return 0) so the
        // guest disables audio through its own graceful path — used to
        // isolate the heap corruption that fires when audio is active.
        static const bool fail_open_ = getenv("BIFROST_AUDIO_FAIL_OPEN") != nullptr;
        if (fail_open_) {
            if (obtained) {
                uint8_t z[32] = {0};
                I.mem->write(obtained, z, 32);
            }
            tr(0); return 0;   // SDL_OpenAudioDevice: 0 = no device
        }
        if (!desired || !I.engine) { tr(-1); return -1; }
        const int32_t  freq = (int32_t)rd32(I, desired + 0);
        const uint16_t ftag = rd16(I, desired + 4);
        uint8_t chb = 0; I.mem->read(desired + 6, &chb, 1);
        const uint8_t ch = chb ? chb : 2;
        const uint16_t samples_per_cb = rd16(I, desired + 8);
        const uint64_t cb_fn = rd64(I, desired + 16);
        const uint64_t cb_ud = rd64(I, desired + 24);
        const uint32_t fmt = sdl_fmt_map(ftag);
        const uint8_t  sz  = fmt_size(fmt);
        const uint64_t h = ++I.next_sdl_dev_;   // real SDL: first id = 1
        std::unique_lock<std::recursive_mutex> pmu(I.pump_mu);
        auto& slot = I.sdl_devs_[h];
        slot.fmt = fmt; slot.rate = (freq > 0) ? (uint32_t)freq : 44100;
        slot.ch = ch; slot.size = sz;
        slot.cb_fn = cb_fn; slot.cb_ud = cb_ud;
        slot.frames_per_cb = samples_per_cb ? samples_per_cb : 1024;
        if (cb_fn) {
            slot.bounce_bytes = (size_t)slot.frames_per_cb * slot.ch * slot.size;
            slot.bounce = I.mem->mmap_alloc(slot.bounce_bytes);
            if (!slot.bounce) { I.sdl_devs_.erase(h); tr(-ENOMEM); return -ENOMEM; }
            // Callbacks are fired INLINE from audio-thunk dispatches on
            // the guest thread (see run_due_callbacks) — never from a
            // host pump thread (the old pump corrupted guest state).
        // TEMP BISECT (BIFROST_AUDIO_NO_CB=1): open the device but never
        // fire callbacks — separates device-open path from callback path.
        if (getenv("BIFROST_AUDIO_NO_CB")) { tr(0); return 0; }
            slot.cb_scheduled = true;
            slot.next_cb_us = 0;   // due immediately; pacing takes over
        }
        pmu.unlock();
        slot.engine_stream = I.engine
            ? I.engine->stream_open(fmt, slot.rate, slot.ch) : 0;
        if (I.engine && !slot.engine_stream) {
            if (slot.bounce) {
                I.mem->untrack_allocation(slot.bounce, slot.bounce_bytes);
                slot.bounce = 0;
            }
            I.sdl_devs_.erase(h);
            tr(-ENOMEM);
            return -ENOMEM;
        }
        if (obtained) {
            wr32(I, obtained + 0, slot.rate);
            I.mem->write(obtained + 4, &ftag, 2);
            I.mem->write(obtained + 6, &slot.ch, 1);
            uint8_t silence = (fmt == PCM_FMT_U8) ? 128 : 0;
            I.mem->write(obtained + 7, &silence, 1);
            uint16_t samples = (uint16_t)slot.frames_per_cb;
            I.mem->write(obtained + 8, &samples, 2);
            uint16_t padw = 0; I.mem->write(obtained + 10, &padw, 2);
            wr32(I, obtained + 12,
                 (uint32_t)(slot.frames_per_cb * slot.ch * slot.size));
            wr64(I, obtained + 16, cb_fn);
            wr64(I, obtained + 24, cb_ud);
        }
        tr(is_dev ? (int64_t)h : 0);
        return is_dev ? (int64_t)h : 0;   // SDL_OpenAudio returns 0 on success
    }
    if (name == "SDL_CloseAudioDevice") {
        std::lock_guard<std::recursive_mutex> pmu(I.pump_mu);
        auto it = I.sdl_devs_.find(R(0));
        if (it != I.sdl_devs_.end()) {
            it->second.stop = true;
            if (it->second.pump.joinable()) it->second.pump.join();
            if (it->second.engine_stream && I.engine)
                I.engine->stream_close(it->second.engine_stream);
            if (it->second.bounce)
                I.mem->untrack_allocation(it->second.bounce, it->second.bounce_bytes);
            I.sdl_devs_.erase(it);
        }
        tr(0); return 0;
    }
    if (name == "SDL_PauseAudioDevice" || name == "SDL_PauseAudio") {
        // "SDL_PauseAudioDevice" has 'D' at index 14 (SDL_PauseAudio
        // uses the implicit device 1).
        uint64_t dev = (name[14] == 'D') ? R(0) : 1;
        uint8_t pause = (uint8_t)R(1);
        auto it = I.sdl_devs_.find(dev);
        if (it != I.sdl_devs_.end()) {
            it->second.paused = pause != 0;
            // Per-device gate in the engine mixer too (real SDL: paused
            // logical devices are skipped, others keep playing).
            if (it->second.engine_stream && I.engine)
                I.engine->stream_pause(it->second.engine_stream, pause != 0);
            if (!pause) {
                // Resume cleanly: no callback burst, drain backlog now.
                it->second.next_cb_us =
                    (uint64_t)std::chrono::duration_cast<
                        std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                            .count();
                run_due_callbacks();
                top_up_queue(it->second);
            }
        }
        tr(0); return 0;
    }
    if (name == "SDL_QueueAudio") {
        uint64_t dev = R(0), data = R(1);
        uint32_t len = (uint32_t)R(2);
        auto it = I.sdl_devs_.find(dev);
        int64_t rc = -1;
        if (it != I.sdl_devs_.end() && data && len && I.engine) {
            run_due_callbacks();
            // Real-SDL semantics: buffer the whole request on the device
            // backlog and let top_up_queue_ feed the ring in order. No
            // samples are dropped, and GetQueuedAudioSize reports the
            // true remaining backlog (guests key their refill logic off
            // it). Defensive 256 MiB cap — real SDL is unbounded, but a
            // runaway guest shouldn't OOM the host.
            constexpr size_t kMaxPending = 256u << 20;
            auto& s = it->second;
            top_up_queue(s);
            if (s.pending.size() - s.pending_off + len <= kMaxPending) {
                std::vector<uint8_t> tmp(len);
                I.mem->read(data, tmp.data(), len);
                s.pending.insert(s.pending.end(), tmp.begin(), tmp.end());
                top_up_queue(s);
                rc = 0;
            } else {
                rc = 0;  // over cap: drop like a full device
            }
        }
        tr(rc); return rc;
    }
    if (name == "SDL_GetQueuedAudioSize") {
        uint64_t dev = R(0);
        auto it = I.sdl_devs_.find(dev);
        int64_t r = 0;
        if (it != I.sdl_devs_.end()) {
            run_due_callbacks();
            top_up_queue(it->second);
            // Honest unplayed backlog: our pending buffer PLUS whatever
            // already sits in the device's engine stream ring (converted
            // back to guest input bytes). Real SDL reports everything
            // queued-but-unplayed; under-reporting makes guests that
            // pace generation off this value over-produce.
            r = (int64_t)(it->second.pending.size() - it->second.pending_off);
            if (it->second.engine_stream && I.engine) {
                const size_t frame_sz =
                    (size_t)(it->second.fmt == PCM_FMT_U8 ? 1
                             : (it->second.fmt == PCM_FMT_F32 ? 4 : 2)) *
                    it->second.ch;
                r += (int64_t)(I.engine->stream_queued_frames(
                                   it->second.engine_stream) * frame_sz);
            }
        }
        tr(r); return r;
    }
    if (name == "SDL_ClearQueuedAudio") {
        uint64_t dev = R(0);
        auto it = I.sdl_devs_.find(dev);
        if (it != I.sdl_devs_.end()) {
            it->second.pending.clear();
            it->second.pending_off = 0;
            // PER-DEVICE clear (real SDL): drop only this device's
            // queued frames. The old global engine-ring wipe let one
            // device's clear destroy every other device's buffered audio.
            if (it->second.engine_stream && I.engine)
                I.engine->stream_clear(it->second.engine_stream);
        }
        tr(0); return 0;
    }
    if (name == "SDL_LockAudioDevice" || name == "SDL_UnlockAudioDevice") {
        // Guests bracket shared-state mutation with these (e.g. voice
        // lists the callback consumes) — a natural dispatch point for
        // deferred callbacks, mirroring where real SDL would run them.
        run_due_callbacks();
        tr(0); return 0;
    }
    if (name == "SDL_DequeueAudio" || name == "SDL_AudioInit" ||
        name == "SDL_AudioQuit" || name == "SDL_MixAudioFormat") {
        tr(0); return 0;
    }
    // Legacy SDL1-style global-device variants (vkQuake uses these).
    if (name == "SDL_LockAudio" || name == "SDL_UnlockAudio") {
        tr(0); return 0;
    }
    if (name == "SDL_CloseAudio") {
        std::lock_guard<std::recursive_mutex> pmu(I.pump_mu);
        auto it = I.sdl_devs_.find(1);
        if (it != I.sdl_devs_.end()) {
            it->second.stop = true;
            if (it->second.pump.joinable()) it->second.pump.join();
            if (it->second.engine_stream && I.engine)
                I.engine->stream_close(it->second.engine_stream);
            if (it->second.bounce)
                I.mem->untrack_allocation(it->second.bounce, it->second.bounce_bytes);
            I.sdl_devs_.erase(it);
        }
        tr(0); return 0;
    }
    if (name == "SDL_GetCurrentAudioDriver") {
        static uint64_t drvbuf = 0;
        if (!drvbuf) drvbuf = I.mem->mmap_alloc(32);
        if (!drvbuf) { tr(-ENOMEM); return -ENOMEM; }
        const char* n = "bifrost";
        I.mem->write(drvbuf, n, strlen(n) + 1);
        tr((int64_t)drvbuf); return (int64_t)drvbuf;
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
        const uint64_t h = 0xA6000000ull + ++I.next_alsa_pcm_;
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
                // Lazily create this pcm's engine stream (fmt/rate/ch are
                // only final once hw_params have been set).
                if (!it->second.engine_stream)
                    it->second.engine_stream = I.engine->stream_open(
                        it->second.fmt, it->second.rate, it->second.ch);
                ssize_t fr = it->second.engine_stream
                    ? I.engine->stream_write(it->second.engine_stream,
                                             it->second.fmt, it->second.rate,
                                             it->second.ch, tmp.data(), bytes)
                    : -1;
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
        // (server, dev, dir, streamname, spec*, map*, attr*, int *error)
        const uint64_t spec = R(4), errp = R(7);
        const uint64_t h = 0xA7000000ull + ++I.next_pulse_;
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
        // libpulse-simple's last param is `int *error` (ONE pointer
        // level) — success means *error = 0. Never dereference errp
        // itself; that wrote through garbage guest memory.
        if (errp) wr32(I, errp, 0);
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
            if (!it->second.engine_stream)
                it->second.engine_stream = I.engine->stream_open(
                    it->second.fmt, it->second.rate, it->second.ch);
            ssize_t fr = it->second.engine_stream
                ? I.engine->stream_write(it->second.engine_stream,
                                         it->second.fmt, it->second.rate,
                                         it->second.ch, tmp.data(), bytes)
                : -1;
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
            uint64_t id = 0xA9000000ull + ++I.next_al_buf_;
            I.al_bufs_[id] = AudioThunkImpl::AlBuffer{};
            wr32(I, ids + (uint64_t)i * 4, (uint32_t)id);
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
            uint64_t id = 0xA9800000ull + ++I.next_al_src_;
            I.al_srcs_[id] = AudioThunkImpl::AlSource{};
            wr32(I, ids + (uint64_t)i * 4, (uint32_t)id);
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
        const uint64_t h = 0xAA000000ull + ++I.next_aa_builder_;
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
        const uint64_t h = 0xAA100000ull + ++I.next_aa_stream_;
        std::unique_lock<std::recursive_mutex> pmu(I.pump_mu);
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
            // Inline deferral (same model as SDL): callbacks fire from
            // audio-thunk dispatches on the guest thread (see
            // run_due_callbacks) — never from a host pump thread (async
            // invocation corrupted guest state).
            s.cb_scheduled = true;
            s.next_cb_us = 0;
        }
        s.engine_stream = I.engine
            ? I.engine->stream_open(s.fmt, s.rate, s.ch) : 0;
        pmu.unlock();
        tr(0); return 0;   // AAUDIO_OK
    }
    if (name == "AAudioStream_requestStart") {
        auto it = I.aa_streams_.find(R(0));
        if (it != I.aa_streams_.end()) {
            it->second.paused = false;
            if (it->second.engine_stream && I.engine)
                I.engine->stream_pause(it->second.engine_stream, false);
        }
        tr(0); return 0;
    }
    if (name == "AAudioStream_requestPause" || name == "AAudioStream_requestStop") {
        auto it = I.aa_streams_.find(R(0));
        if (it != I.aa_streams_.end()) {
            it->second.paused = true;
            if (it->second.engine_stream && I.engine)
                I.engine->stream_pause(it->second.engine_stream, true);
        }
        tr(0); return 0;
    }
    if (name == "AAudioStream_close") {
        std::lock_guard<std::recursive_mutex> pmu(I.pump_mu);
        auto it = I.aa_streams_.find(R(0));
        if (it != I.aa_streams_.end()) {
            it->second.stop = true;
            if (it->second.pump.joinable()) it->second.pump.join();
            if (it->second.engine_stream && I.engine)
                I.engine->stream_close(it->second.engine_stream);
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
            ssize_t fr = it->second.engine_stream
                ? I.engine->stream_write(it->second.engine_stream,
                                         it->second.fmt, it->second.rate,
                                         it->second.ch, tmp.data(), bytes)
                : -1;
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
            AudioThunkImpl::SlObject* o = it->second;
            // Players configure format via SLDataSource; default S16/44100/2.
            // Per-player engine stream: concurrent players mix in the
            // device callback (real OpenSL semantics).
            if (!o->engine_stream)
                o->engine_stream = I.engine->stream_open(PCM_FMT_S16, 44100, 2);
            if (o->engine_stream)
                I.engine->stream_write(o->engine_stream, PCM_FMT_S16, 44100,
                                       2, tmp.data(), bytes);
            rc = 0;   // SL_RESULT_SUCCESS
            // Fire the registered callback INLINE (guest thread): mirrors
            // BufferQueue semantics closely enough for streaming players.
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
