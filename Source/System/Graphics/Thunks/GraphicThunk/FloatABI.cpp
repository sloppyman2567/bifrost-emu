// dispatch() stage 3: typed legacy GL, float/double returns and the AAPCS64
// floating-point argument paths (double-only, float-only, mixed int+float).
#include "System/Graphics/Thunks/GraphicThunk/GraphicThunkInternal.hpp"
namespace arm64emu {
std::optional<int64_t> GraphicThunk::dispatch_float_abi_(CPU& cpu, const SymbolEntry& entry) {
    if (entry.spec && entry.spec->policy == thunk::Policy::GL_TYPED) {
        dispatch_legacy_gl(*impl_->mem, cpu, *entry.spec, entry.host_fn);
        return 0;
    }

    // Return type is independent of argument type: the math APIs return
    // in v0, whereas GL setters with the same arguments return void.
    if (entry.spec && (entry.spec->ret == thunk::RetKind::FLOAT ||
                       entry.spec->ret == thunk::RetKind::DOUBLE)) {
        const char* shape = entry.spec->args;
        uint64_t bits = 0;
        if (entry.spec->ret == thunk::RetKind::FLOAT) {
            float a, b, result;
            std::memcpy(&a, &cpu.v_lo[0], 4);
            std::memcpy(&b, &cpu.v_lo[1], 4);
            if (!strcmp(shape, "f")) result = reinterpret_cast<float (*)(float)>(entry.host_fn)(a);
            else if (!strcmp(shape, "ff")) result = reinterpret_cast<float (*)(float, float)>(entry.host_fn)(a, b);
            else return -ENOSYS;
            std::memcpy(&bits, &result, 4);
        } else {
            double a, b, result;
            std::memcpy(&a, &cpu.v_lo[0], 8);
            std::memcpy(&b, &cpu.v_lo[1], 8);
            if (!strcmp(shape, "d")) result = reinterpret_cast<double (*)(double)>(entry.host_fn)(a);
            else if (!strcmp(shape, "dd")) result = reinterpret_cast<double (*)(double, double)>(entry.host_fn)(a, b);
            else return -ENOSYS;
            std::memcpy(&bits, &result, 8);
        }
        cpu.v_lo[0] = bits;
        cpu.v_hi[0] = 0;
        return 0;
    }
    // SDL's mixed FP APIs also have pointer-width and return-value contracts
    // that cannot be represented by the legacy int32/void GL setter casts.
    if (entry.name == "SDL_CalculateGammaRamp") {
        float gamma; std::memcpy(&gamma, &cpu.v_lo[0], 4);
        uint16_t ramp[256];
        reinterpret_cast<void (*)(float, uint16_t*)>(entry.host_fn)(gamma, ramp);
        if (cpu.regs[0]) impl_->mem->write(cpu.regs[0], ramp, sizeof(ramp));
        cpu.regs[0] = 0;
        return 0;
    }
    if (entry.name == "SDL_RenderSetScale") {
        float x, y;
        std::memcpy(&x, &cpu.v_lo[0], 4); std::memcpy(&y, &cpu.v_lo[1], 4);
        int rc = reinterpret_cast<int (*)(void*, float, float)>(entry.host_fn)(
            reinterpret_cast<void*>(cpu.regs[0]), x, y);
        cpu.regs[0] = static_cast<int64_t>(rc);
        return 0;
    }
    if (entry.name == "SDL_RenderCopyEx") {
        int32_t src[4], dst[4], center[2];
        if (cpu.regs[2]) impl_->mem->read(cpu.regs[2], src, sizeof(src));
        if (cpu.regs[3]) impl_->mem->read(cpu.regs[3], dst, sizeof(dst));
        if (cpu.regs[4]) impl_->mem->read(cpu.regs[4], center, sizeof(center));
        double angle; std::memcpy(&angle, &cpu.v_lo[0], 8);
        using Fn = int (*)(void*, void*, const void*, const void*, double, const void*, int);
        int rc = reinterpret_cast<Fn>(entry.host_fn)(
            reinterpret_cast<void*>(cpu.regs[0]), reinterpret_cast<void*>(cpu.regs[1]),
            cpu.regs[2] ? src : nullptr, cpu.regs[3] ? dst : nullptr, angle,
            cpu.regs[4] ? center : nullptr, static_cast<int>(cpu.regs[5]));
        cpu.regs[0] = static_cast<int64_t>(rc);
        return 0;
    }

    // ── Double-only AAPCS64 path (glOrtho, glClearDepth, …) ─────────
    // Guest AArch64 passes GLdouble args in d0..d{n-1} (low 64 bits of
    // v0..). Host SysV AMD64 expects them in XMM0.. — the C++ cast below
    // places them there automatically.
    if (entry.flags & THUNK_DOUBLE) {
        // Mixed int + double first: the pure-double arm below would drop
        // the integer args (e.g. glfwSetCursorPos's window handle).
        if (entry.flags & THUNK_MIXED_FP) {
            uint64_t iv = cpu.regs[0];
            double dv[2] = {0, 0};
            for (uint8_t i = 0; i < entry.n_float && i < 2; i++) {
                std::memcpy(&dv[i], &cpu.v_lo[i], sizeof(double));
            }
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] dispatch: %s (mixed int+double) "
                        "i0=0x%llx d0=%g d1=%g\n",
                        entry.name.c_str(),
                        static_cast<unsigned long long>(iv), dv[0], dv[1]);
            }
            if (entry.n_stack == 1 && entry.n_float == 2) {
                using Fn = void (*)(void*, double, double);
                reinterpret_cast<Fn>(entry.host_fn)(
                    reinterpret_cast<void*>(iv), dv[0], dv[1]);
            } else if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] mixed int+double shape unsupported for %s\n",
                        entry.name.c_str());
            }
            cpu.regs[0] = 0;
            return 0;
        }
        double dv[8] = {0};
        for (uint8_t i = 0; i < entry.n_float && i < 8; i++) {
            std::memcpy(&dv[i], &cpu.v_lo[i], sizeof(double));
        }
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] dispatch: %s (double×%u) d0=%g d1=%g d2=%g d3=%g\n",
                    entry.name.c_str(), entry.n_float,
                    dv[0], dv[1], dv[2], dv[3]);
        }
        switch (entry.n_float) {
        case 1: {
            using Fn = void (*)(double);
            reinterpret_cast<Fn>(entry.host_fn)(dv[0]);
            break;
        }
        case 2: {
            using Fn = void (*)(double, double);
            reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1]);
            break;
        }
        case 3: {
            using Fn = void (*)(double, double, double);
            reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2]);
            break;
        }
        case 4: {
            using Fn = void (*)(double, double, double, double);
            reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3]);
            break;
        }
        case 5: {
            using Fn = void (*)(double, double, double, double, double);
            reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3], dv[4]);
            break;
        }
        case 6: {
            using Fn = void (*)(double, double, double, double, double, double);
            reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3], dv[4], dv[5]);
            break;
        }
        default: {
            using Fn = void (*)(double, double, double, double, double, double,
                                double, double);
            reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3],
                                                dv[4], dv[5], dv[6], dv[7]);
            break;
        }
        }
        cpu.regs[0] = 0;
        // No GLStateTracker handler consumes double-typed state setters
        // (glOrtho/glClearDepth are not tracked), so skip it here.
        return 0;
    }

    // ── Float-only AAPCS64 path (glClearColor, glVertex3f, …) ────────
    if (entry.n_float > 0 && !(entry.flags & THUNK_MIXED_FP)
        && !(entry.flags & THUNK_GET_PROC)) {
        float fv[8] = {0};
        for (uint8_t i = 0; i < entry.n_float && i < 8; i++) {
            std::memcpy(&fv[i], &cpu.v_lo[i], sizeof(float));
        }
        uint64_t local_args[12] = {0};
        for (int i = 0; i < 8; i++) local_args[i] = cpu.regs[i];
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] dispatch: %s (fp×%u) f0=%g f1=%g f2=%g f3=%g\n",
                    entry.name.c_str(), entry.n_float,
                    fv[0], fv[1], fv[2], fv[3]);
        }
        switch (entry.n_float) {
        case 1: {
            using Fn = void (*)(float);
            reinterpret_cast<Fn>(entry.host_fn)(fv[0]);
            break;
        }
        case 2: {
            using Fn = void (*)(float, float);
            reinterpret_cast<Fn>(entry.host_fn)(fv[0], fv[1]);
            break;
        }
        case 3: {
            using Fn = void (*)(float, float, float);
            reinterpret_cast<Fn>(entry.host_fn)(fv[0], fv[1], fv[2]);
            break;
        }
        default: {
            using Fn = void (*)(float, float, float, float);
            reinterpret_cast<Fn>(entry.host_fn)(fv[0], fv[1], fv[2], fv[3]);
            break;
        }
        }
        cpu.regs[0] = 0;
        if (impl_->gl_state_tracker_ && entry.tracks_state) {
            impl_->gl_state_tracker_->track_state_change(entry.name, local_args, fv, entry.n_float);
        }
        return 0;
    }

    // ── Mixed int + float (glUniform*f, glTexParameterf, …) ─────────
    // n_stack holds the integer arity (x0..); n_float holds float arity
    // (v0..). Used only when THUNK_MIXED_FP is set.
    if (entry.flags & THUNK_MIXED_FP) {
        uint64_t iv[4] = {0};
        float fv[4] = {0};
        uint8_t ni = entry.n_stack;
        if (ni > 4) ni = 4;
        for (uint8_t i = 0; i < ni; i++) iv[i] = cpu.regs[i];
        for (uint8_t i = 0; i < entry.n_float && i < 4; i++) {
            std::memcpy(&fv[i], &cpu.v_lo[i], sizeof(float));
        }
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] dispatch: %s (mixed int×%u fp×%u) "
                    "i0=%lld f0=%g\n",
                    entry.name.c_str(), ni, entry.n_float,
                    static_cast<long long>(iv[0]), fv[0]);
        }
        // glSampleCoverage(GLfloat value, GLboolean invert) is the one
        // registered mixed GL setter whose float precedes its integer in
        // the AArch64 prototype. The generic mixed path handles integer
        // arguments first, so preserve this function's actual ABI order.
        if (entry.name == "glSampleCoverage" && ni == 1 &&
            entry.n_float == 1) {
            using Fn = void (*)(float, uint8_t);
            reinterpret_cast<Fn>(entry.host_fn)(
                fv[0], static_cast<uint8_t>(iv[0]));
        } else if (ni == 1 && entry.n_float == 1) {
            using Fn = void (*)(int32_t, float);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<int32_t>(iv[0]), fv[0]);
        } else if (ni == 1 && entry.n_float == 2) {
            using Fn = void (*)(int32_t, float, float);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<int32_t>(iv[0]), fv[0], fv[1]);
        } else if (ni == 1 && entry.n_float == 3) {
            using Fn = void (*)(int32_t, float, float, float);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<int32_t>(iv[0]), fv[0], fv[1], fv[2]);
        } else if (ni == 1 && entry.n_float >= 4) {
            using Fn = void (*)(int32_t, float, float, float, float);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<int32_t>(iv[0]), fv[0], fv[1], fv[2], fv[3]);
        } else if (ni == 2 && entry.n_float == 1) {
            using Fn = void (*)(uint32_t, uint32_t, float);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<uint32_t>(iv[0]),
                static_cast<uint32_t>(iv[1]), fv[0]);
        } else if (ni == 2 && entry.n_float == 4) {
            // glBitmap(width, height, xorig, yorig, xmove, ymove, bits):
            // two count ints in x0/x1, four floats in v0..v3, and the
            // bitmap pointer in x2 (aapcs64: next free GPR after x0/x1 —
            // floats travel in SIMD regs, not GPRs). The old arm asked
            // for ni==3, which the iiffffz spec never produces, so every
            // glBitmap fell into the unsupported no-op below.
            uint64_t bw = iv[0], bh = iv[1];
            uint64_t brow = (bw + 7) / 8;
            uint64_t bstride = (brow + 3) & ~(uint64_t)3;
            uint64_t bsz = bh ? (bh - 1) * bstride + brow : 0;
            uint64_t bits_guest = cpu.regs[2];
            const void* bits = nullptr;
            std::vector<uint8_t> bbounce;
            if (impl_->mem) {
                uint8_t* hp = impl_->mem->guest_to_host_ptr(bits_guest);
                if (hp) {
                    bits = hp;
                } else if (bsz > 0 && bsz < (1ull << 20)) {
                    try {
                        bbounce.resize(static_cast<size_t>(bsz));
                        impl_->mem->read(bits_guest, bbounce.data(),
                                         static_cast<size_t>(bsz));
                        bits = bbounce.data();
                    } catch (...) { bits = nullptr; }
                }
            }
            using Fn = void (*)(uint32_t, uint32_t, float, float, float,
                                float, const void*);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<uint32_t>(iv[0]), static_cast<uint32_t>(iv[1]),
                fv[0], fv[1], fv[2], fv[3], bits);
        } else {
            // Unsupported mixed shape — no-op rather than corrupt.
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] mixed FP shape unsupported for %s\n",
                        entry.name.c_str());
            }
        }
        cpu.regs[0] = 0;
        if (impl_->gl_state_tracker_ && entry.tracks_state) {
            uint64_t mixed_args[12] = {0};
            mixed_args[0] = iv[0];
            impl_->gl_state_tracker_->track_state_change(entry.name, mixed_args, fv, entry.n_float);
        }
        return 0;
    }

    return std::nullopt;
}
}  // namespace arm64emu
