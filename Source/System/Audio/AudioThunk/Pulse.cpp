// PulseAudio simple API on AudioEngine streams.
#include "System/Audio/AudioThunk/AudioThunkInternal.hpp"
namespace arm64emu {
#define R(i) (cpu.regs[i])
std::optional<int64_t> AudioDispatch::pulse() {
    // ══ PulseAudio simple API ═══════════════════════════════════════
    if (name == "pa_simple_new") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        // (server, dev, dir, streamname, spec*, map*, attr*, int *error)
        const uint64_t spec = R(4), errp = R(7);
        const uint64_t h = 0xA7000000ull + ++I.next_pulse_;
        auto& ps = I.pulse_streams_[h];
        if (spec) {
            uint32_t pf = rd32(I, spec + 0);
            uint32_t pr = rd32(I, spec + 4);
            uint8_t  pc = 0; I.mem->read(spec + 8, &pc, 1);
            switch (pf) {   // pa_sample_format_t (values from pulse/sample.h)
                case 0: ps.fmt = PCM_FMT_U8; break;       // PA_SAMPLE_U8
                case 5: case 6: ps.fmt = PCM_FMT_F32; break;  // FLOAT32LE/BE
                case 7: case 8: ps.fmt = PCM_FMT_S32; break;  // S32LE/BE
                case 11: case 12: ps.fmt = PCM_FMT_S24; break; // S24_32LE/BE
                                                              // (32-bit
                                                              // containers;
                                                              // packed S24LE/BE
                                                              // unsupported)
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
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.pulse_streams_.find(R(0));
        int64_t rc = -EIO;
        if (it != I.pulse_streams_.end() && I.engine) {
            uint64_t data = R(1);
            size_t bytes = (size_t)R(2);
            // Unbounded host allocation on a guest-controlled size: clamp
            // to 64 MiB (matches the snd_pcm_writei cap and the 256 MiB
            // queue backlog bound in spirit).
            constexpr size_t kMaxBytes = 64u << 20;
            if (bytes > kMaxBytes) { tr(-ENOMEM); return -ENOMEM; }
            std::vector<uint8_t> tmp;
            try {
                tmp.resize(bytes);
                if (bytes) I.mem->read(data, tmp.data(), bytes);
            } catch (...) { tr(-EFAULT); return -EFAULT; }
            if (!it->second.engine_stream)
                it->second.engine_stream = I.engine->stream_open(
                    it->second.fmt, it->second.rate, it->second.ch);
            // Blocking semantics like writei above: pa_simple is a
            // synchronous API — wait out a full ring (bounded) instead
            // of dropping. Empty acceptance after the budget is -EAGAIN.
            size_t acc = it->second.engine_stream
                ? stream_write_wait(it->second.engine_stream,
                                    it->second.fmt, it->second.rate,
                                    it->second.ch, tmp.data(), bytes)
                : 0;
            if (!it->second.engine_stream) rc = -EIO;
            else if (acc == 0 && bytes != 0) rc = -EAGAIN;
            else rc = 0;
        }
        tr(rc); return rc;
    }
    if (name == "pa_simple_drain" || name == "pa_simple_flush") { tr(0); return 0; }
    if (name == "pa_simple_free") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.pulse_streams_.find(R(0));
        if (it != I.pulse_streams_.end()) {
            if (it->second.engine_stream && I.engine)
                I.engine->stream_close(it->second.engine_stream);
            I.pulse_streams_.erase(it);
        }
        tr(0); return 0;
    }
    if (name == "pa_simple_get_latency") { tr(0); return 0; }
    if (name == "pa_strerror") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        static uint64_t pbuf = 0;
        if (!pbuf) pbuf = I.mem->mmap_alloc(64);
        const char* s = "audio-thunk";
        I.mem->write(pbuf, s, strlen(s) + 1);
        tr((int64_t)pbuf); return (int64_t)pbuf;
    }

    return std::nullopt;
}
#undef R
}  // namespace arm64emu
