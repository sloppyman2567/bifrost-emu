// audio/audio.h — Audio backend for bifrost-emu.
//
// Multi-stream mixer modeled on real SDL2/SDL3 (1.5.5-alpha rewrite):
// ONE physical host device (SDL2 callback / OSS /dev/dsp / headless) plus
// N logical streams — one per guest audio device. Each stream has its own
// ring of float frames at the DEVICE rate (input is format-converted and
// linearly resampled at push time). The device callback sums every
// non-paused stream, clips, and converts to the device format — exactly
// how SDL mixes simultaneous logical devices. Backpressure is per-stream
// (ring full ⇒ write accepts 0), so one chatty guest device can never
// starve another (the old shared-ring design let music permanently pin
// the global level and silently kill sfx).
//
//   1. SDL2 (build with USE_SDL2=1) — preferred backend.
//   2. OSS (/dev/dsp) — fallback real-time playback.
//   3. Headless — bytes_pushed() counter + optional WAV dump; used by
//      automated tests.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
namespace arm64emu {
// Sample-format tags for the push APIs. Kept as plain constants so thunk
// arms can pass SDL/ALSA/OpenAL/AAudio enum values through a tiny mapping
// without pulling host headers in.
enum PcmFormat : uint32_t {
    PCM_FMT_U8  = 1,   // unsigned 8-bit
    PCM_FMT_S16 = 2,   // signed 16-bit host-endian
    PCM_FMT_S24 = 3,   // signed 24-bit in 4-byte container (low 3 bytes)
    PCM_FMT_F32 = 4,   // IEEE float32
    PCM_FMT_S32 = 5,   // signed 32-bit host-endian
};
inline uint8_t pcm_fmt_size(uint32_t f) {
    switch (f) {
        case PCM_FMT_U8: return 1;
        case PCM_FMT_S16: return 2;
        case PCM_FMT_F32: return 4;
        case PCM_FMT_S32: return 4;  // 32-bit container
        case PCM_FMT_S24: return 4;  // 24-in-32 container
        default: return 0;
    }
}
class Audio {
public:
    Audio();
    ~Audio();
    // ── Logical stream API (one per guest audio device) ───────────────
    // Opens (lazily) the physical device on first use; its format comes
    // from the FIRST stream. Returns a stream id > 0, or 0 on failure.
    int stream_open(uint32_t fmt, uint32_t rate, uint8_t ch);
    void stream_close(int id);
    void stream_pause(int id, bool paused);
    bool stream_paused(int id);
    // Drop any queued-but-unplayed frames of THIS stream only.
    void stream_clear(int id);
    // Frames currently queued-but-unplayed in this stream's ring
    // (GetQueuedAudioSize honest-backlog support).
    size_t stream_queued_frames(int id);
    // Format-converting push into the stream's own ring. `data` is
    // interleaved samples in `fmt` at `rate` Hz / `ch` channels. Converts
    // to float at the device rate (linear resample when needed) and
    // stores in the stream ring. Returns INPUT FRAMES ACCEPTED — 0 when
    // the stream ring is full (caller retries later). All-or-nothing per
    // call so callers can advance their buffers by exact input chunks.
    ssize_t stream_write(int id, uint32_t fmt, uint32_t rate, uint8_t ch,
                         const uint8_t* data, size_t bytes);
    // Total PCM bytes accepted through any push path since construction
    // (test/diagnostic counter — headless tests key off this).
    uint64_t bytes_pushed() const { return bytes_pushed_.load(std::memory_order_relaxed); }
    // ── Legacy single-device API (/dev/dsp VFS node + old callers) ────
    // Routes onto an implicit legacy stream (id 1) with this format.
    bool open(uint32_t sample_rate, uint8_t channels, uint8_t sample_size);
    void close();
    ssize_t write(const uint8_t* data, size_t len);
    ssize_t read(uint8_t* buf, size_t len);
    int ioctl(uint32_t cmd, uint64_t arg);
    bool ready() const { return opened_; }
    uint32_t sample_rate() const { return sample_rate_; }
    uint8_t channels() const { return channels_; }
    uint8_t sample_size() const { return sample_size_; }
    const char* backend_name() const;
    // Dump the audio buffer to a WAV file (for headless testing).
    // Captures ALL bytes written since open(), even if a real audio
    // device was active (useful for regression testing).
    bool dump_to_wav(const std::string& path);
private:
    // One logical stream. Producers (guest threads) and the consumer
    // (device callback) all take mu_ — traffic is tiny (≤16 KiB pushes
    // every few ms vs ~46 pulls/s), correctness beats lock-freedom here.
    struct Stream {
        uint32_t fmt = PCM_FMT_S16;     // input format of pushes
        uint32_t rate = 44100;
        uint8_t ch = 2;
        std::vector<float> ring;        // interleaved float frames @ dev rate
        size_t head = 0, tail = 0;      // monotonic frame counters
        size_t cap_frames = 0;          // power of two
        bool paused = false;
    };
    bool opened_ = false;               // physical device open
    uint32_t sample_rate_ = 44100;
    uint8_t channels_ = 2;
    uint8_t sample_size_ = 2;           // device bytes/sample
    int fd_ = -1;                       // raw OSS PCM output fd (-1 if none)
    int next_stream_id_ = 0;            // monotonic; 0 = invalid
    std::map<int, Stream> streams_;     // under mu_
    std::vector<uint8_t> buffer_;       // accumulated PCM for WAV dump
    std::atomic<uint64_t> bytes_pushed_{0};
    std::mutex mu_;                     // protects streams_, buffer_, formats
    static constexpr size_t kStreamRingBytes = 128 * 1024;  // ~370 ms f32 stereo
    int legacy_stream_ = 0;             // created on demand by open()/write()
    // ── physical device state ─────────────────────────────
    uint32_t sdl_audio_dev_ = 0;        // 0 = no SDL2 device
    std::mutex sdl_mu_;                 // protects sdl_audio_dev_ open/close
    // Open the SDL2 audio device with the current sample_rate/channels/
    // sample_size. Returns true on success. Sets sdl_audio_dev_.
    bool open_sdl2_();
    // Close the SDL2 audio device. No-op if not open.
    void close_sdl2_();
    bool open_device_locked_();         // first caller fixes device format
    void close_device_locked_();
    // Ensure `legacy_stream_` exists with the given format. mu_ held.
    int ensure_legacy_stream_locked_(uint32_t rate, uint8_t ch, uint8_t size);
    // Convert input frames and append to a stream ring (all-or-nothing;
    // returns INPUT frames accepted, 0 = ring full). mu_ held.
    ssize_t push_frames_locked_(Stream& s, uint32_t fmt, uint32_t rate,
                                uint8_t ch, const uint8_t* data,
                                size_t frames_in);
    // Device callback: sum non-paused streams → clip → device format.
    static void mixer_callback_(void* userdata, uint8_t* stream, int len);
    // Convert `frames_in` input frames to float @ device rate into out.
    // Caller holds mu_. Shared by stream_write and legacy path.
    void convert_to_f32_(uint32_t fmt, uint32_t rate, uint8_t ch,
                         const uint8_t* data, size_t frames_in,
                         std::vector<float>& out);
    std::vector<float> conv_;
    std::vector<float> mix_;    // mixer callback scratch (under mu_)
};
} // namespace arm64emu
