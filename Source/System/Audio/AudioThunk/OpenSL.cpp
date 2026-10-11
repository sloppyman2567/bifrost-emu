// OpenSL ES engine/player objects built from synthetic guest vtables.
#include "System/Audio/AudioThunk/AudioThunkInternal.hpp"
namespace arm64emu {
#define R(i) (cpu.regs[i])
std::optional<int64_t> AudioDispatch::opensl() {
    // ══ OpenSL ES (synthetic vtables) ═══════════════════════════════
    // Object model: SLObjectItf is a pointer W where *W is a vtable of
    // function pointers into our trampolines. IID blobs resolve via the
    // SL_IID_* symbol rows (pointer equality against their trampoline
    // addresses — guests never dereference them).
    auto make_vtable = [this](const std::vector<std::string>& syms,
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
        std::lock_guard<std::recursive_mutex> g(I.mu);
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
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.sl_by_word_.find(R(0));
        if (it != I.sl_by_word_.end()) it->second->realized = true;
        tr(0); return 0;
    }
    if (name == "__osl_destroy") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.sl_by_word_.find(R(0));
        if (it != I.sl_by_word_.end()) {
            it->second->realized = false;
            I.sl_bq_by_word_.erase(it->second->bq_itf_word);
            I.sl_play_by_word_.erase(it->second->play_itf_word);
        }
        tr(0); return 0;
    }
    if (name == "__osl_getinterface") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
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
        std::lock_guard<std::recursive_mutex> g(I.mu);
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
        std::lock_guard<std::recursive_mutex> g(I.mu);
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
        uint64_t self = R(0);
        uint64_t cb_fn = 0, cb_ctx = 0;
        int64_t rc = 1;   // SL_RESULT_PARAMETER_INVALID
        {
            std::lock_guard<std::recursive_mutex> g(I.mu);
            auto it = I.sl_bq_by_word_.find(self);
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
                cb_fn = o->bq_cb_fn;
                cb_ctx = o->bq_cb_ctx;
            }
        }
        // Fire the registered callback OUTSIDE the lock (guest code may
        // call back into audio-thunk dispatches).
        if (rc == 0 && cb_fn && I.runner) {
            int64_t ia[2] = { (int64_t)self, (int64_t)cb_ctx };
            I.runner(cpu, cb_fn, ia, 2);
        }
        tr(rc); return rc;
    }
    if (name == "__osl_bq_register") {
        std::lock_guard<std::recursive_mutex> g(I.mu);
        auto it = I.sl_bq_by_word_.find(R(0));
        if (it != I.sl_bq_by_word_.end()) {
            it->second->bq_cb_fn = R(1);
            it->second->bq_cb_ctx = R(2);
        }
        tr(0); return 0;
    }
    if (name == "__osl_play_setstate") { tr(0); return 0; }

    return std::nullopt;
}
#undef R
}  // namespace arm64emu
