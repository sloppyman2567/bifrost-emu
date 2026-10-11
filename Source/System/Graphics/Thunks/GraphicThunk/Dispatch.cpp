// GraphicThunk::dispatch — SVC entry: runs the policy stages, then the generic
// integer/pointer path (bounces, state tracking, writeback, presentation).
#include "System/Graphics/Thunks/GraphicThunk/GraphicThunkInternal.hpp"
namespace arm64emu {
int64_t GraphicThunk::dispatch(CPU& cpu, uint32_t symbol_id) {
    if (!impl_ || !impl_->enabled || !impl_->initialized) {
        return -ENOSYS;
    }
    if ((symbol_id & GraphicThunk::ID_MASK) != GraphicThunk::ID_BASE_GRAPHICS) {
        return -ENOENT;
    }
    uint32_t local_id = symbol_id - GraphicThunk::ID_BASE_GRAPHICS;
    if (local_id >= impl_->id_to_idx_.size()) {
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] dispatch: unknown symbol_id=%u\n", symbol_id);
        }
        return -ENOENT;
    }
    auto [lib_idx, ent_idx] = impl_->id_to_idx_[local_id];
    const auto& entry = impl_->libs_[lib_idx].entries[ent_idx];

#if defined(BIFROST_THUNK_HAVE_SDL2)
    struct SdlCpuScope {
        CPU* previous;
        SdlCpuScope(CPU& current):previous(GraphicThunkImpl::sdl_dispatch_cpu_) {
            GraphicThunkImpl::sdl_dispatch_cpu_ = &current;
        }
        ~SdlCpuScope() { GraphicThunkImpl::sdl_dispatch_cpu_ = previous; }
    } sdl_cpu_scope(cpu);
    if (entry.spec && entry.spec->policy == thunk::Policy::SDL_SURFACE) {
        uint64_t window = cpu.regs[0];
        cpu.regs[0] = impl_->surface_bridge.dispatch(*impl_->mem, cpu, entry.name);
        if (cpu.regs[0] == 0 && (entry.name == "SDL_UpdateWindowSurface" || entry.name == "SDL_UpdateWindowSurfaceRects"))
            window_stats::sdl_present(reinterpret_cast<void*>(window), "SDL surface");
        return 0;
    }
    if (entry.name == "SDL_DestroyWindow") window_stats::forget(reinterpret_cast<void*>(cpu.regs[0]));
    if (entry.name == "SDL_DestroyWindow" || entry.name == "SDL_SetWindowSize")
        impl_->surface_bridge.forget_window(*impl_->mem, cpu.regs[0]);
    if (entry.name == "SDL_Quit") impl_->surface_bridge.clear(*impl_->mem);
#else
    if (entry.spec && entry.spec->policy == thunk::Policy::SDL_SURFACE) {
        cpu.regs[0] = 0;
        return 0;  // no SDL ABI support: never expose a host struct
    }
#endif
    if (auto handled = dispatch_special_policies_(cpu, entry)) return *handled;

    // ── Android surface interception (eglGetDisplay) ─────────────────
    // On a Wayland host, EGL_DEFAULT_DISPLAY must wrap SDL's wl_display —
    // the SAME connection the ANativeWindow's wl_surface lives on — or
    // Mesa rejects eglCreateWindowSurface cross-connection.
    static const bool no_disp_subst =
        (getenv("BIFROST_NO_DISPLAY_SUBST") != nullptr);
    if (entry.name == "eglGetDisplay" && !no_disp_subst) {
        auto& mgr = frost::AndroidSurfaceManager::instance();
        if (cpu.regs[0] == 0 && mgr.ready()) {
            if (void* display = mgr.native_display())
                cpu.regs[0] = reinterpret_cast<uint64_t>(display);
        }
    }
    // ── Android surface interception (eglCreateWindowSurface) ────────
    // When the native-window argument is an ANativeWindow shim handle
    // (Android surface layer), substitute the HOST native window value so
    // host EGL creates a real window surface. eglSwapBuffers then presents
    // through the normal host path with no further interception.
    uint64_t android_surface_handle = 0;
    if (entry.name == "eglCreateWindowSurface") {
        auto& mgr = frost::AndroidSurfaceManager::instance();
        if (frost::AndroidSurfaceManager::is_shim(cpu.regs[2])) {
            android_surface_handle = cpu.regs[2];
            uint64_t hw = mgr.host_native_window(cpu.regs[2]);
            if (hw == 0) {
                // No host window backing the shim — EGL_NO_SURFACE.
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] eglCreateWindowSurface: "
                            "android shim without host window\n");
                cpu.regs[0] = 0;
                return 0;
            }
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] eglCreateWindowSurface: android "
                        "shim 0x%llx -> host window 0x%llx\n",
                        static_cast<unsigned long long>(cpu.regs[2]),
                        static_cast<unsigned long long>(hw));
            }
            cpu.regs[2] = hw;
        }
    }

    if (auto handled = dispatch_guest_callbacks_(cpu, entry)) return *handled;
    if (auto handled = dispatch_float_abi_(cpu, entry)) return *handled;
    // ── Integer/pointer path with optional stack args ────────────────
    // 16-arg ceiling: glCopyImageSubData takes 15 (8 reg + 7 stack).
    // Anything past the ceiling reads as 0 (silent corruption), so the
    // cap must cover the widest table row.
    constexpr int kMaxArgs = 16;
    uint64_t args[kMaxArgs] = {0};
    for (int i = 0; i < 8; i++) args[i] = cpu.regs[i];
    // AAPCS64: args 8+ live on the guest stack at SP, 8-byte slots.
    if (entry.n_stack && impl_->mem) {
        for (uint8_t i = 0; i < entry.n_stack && (8 + i) < kMaxArgs; i++) {
            uint64_t slot = cpu.sp + static_cast<uint64_t>(i) * 8ull;
            impl_->mem->read(slot, &args[8 + i], sizeof(uint64_t));
        }
    }

    // ── SDL_JoystickGetGUID — 16-byte struct return by value ───────────
    // Host SysV x86-64 returns SDL_JoystickGUID (16 bytes, two INTEGER
    // fields) in RAX:RDX; the guest expects it in x0:x1. The row's ARGS is
    // 'i', so the pointer loop never translates the opaque SDL_Joystick*
    // handle — it round-trips as the host pointer SDL_JoystickOpen returned.
    // This arm MUST run before the pointer-args loop.
    if (entry.spec && entry.spec->policy == thunk::Policy::JOY_GUID) {
        struct Guid16 { uint64_t lo, hi; };
        Guid16 g{0, 0};
        if (entry.host_fn) {
            using Fn = Guid16 (*)(uint64_t);
            g = reinterpret_cast<Fn>(entry.host_fn)(cpu.regs[0]);
        }
        cpu.regs[0] = g.lo;
        cpu.regs[1] = g.hi;
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] SDL_JoystickGetGUID(0x%llx) -> %016llx%016llx\n",
                    static_cast<unsigned long long>(args[0]),
                    static_cast<unsigned long long>(g.hi),
                    static_cast<unsigned long long>(g.lo));
        }
        return 0;
    }

    // ── glMapBuffer / glMapBufferRange / glUnmapBuffer / glFlushMappedBufferRange ──
    // 2026-08: the host glMapBuffer returns a HOST pointer the guest cannot
    // deref (address-space mismatch). Instead, bounce through a guest-window
    // allocation: mmap_alloc() hands out guest addresses the guest JIT
    // reads and writes inline. On
    // map, seed the bounce from the host buffer when GL_MAP_READ_BIT is
    // set (contents undefined otherwise / under GL_MAP_INVALIDATE_*); on
    // unmap, copy the bounce back into the host buffer when GL_MAP_WRITE_BIT
    // is set, then free the bounce. GL_MAP_FLUSH_EXPLICIT_BIT ranges are
    // pushed early by the FLUSH_BUFFER arm. Transient map/write/unmap per
    // frame works fully; persistent-coherent-without-explicit-flush stays
    // unsupported (the writes would never land on the host).
    constexpr uint32_t kGLMapReadBit = 0x0001;
    constexpr uint32_t kGLMapWriteBit = 0x0002;
    constexpr uint32_t kGLMapInvalidateRangeBit = 0x0004;
    constexpr uint32_t kGLMapInvalidateBufferBit = 0x0008;
    constexpr uint32_t kGLMapPersistentBit = 0x0040;
    constexpr uint32_t kGLMapCoherentBit = 0x0080;
    constexpr uint32_t kGLBufferSize = 0x8764;
    // PCWFC (Persistent-Coherent Writeback For Coherence): a mapping with
    // GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_MAP_WRITE_BIT is a
    // "serious engine" streaming buffer — the guest maps it ONCE, writes
    // it every frame through the returned pointer, and never unmaps it
    // (glUnmapBuffer on a persistent mapping keeps it valid per GL). The
    // bounce only wrote back on unmap/flush, so the host GPU never saw the
    // writes. sync_persistent_mappings_() pushes every live persistent
    // bounce back to the host buffer, and dispatch() calls it right before
    // any call that consumes buffer data (draws, copies, getSubData,
    // texbuffer) — the practical "coherence" guarantee (host sees writes at
    // the moment the GPU would read them).
    auto sync_persistent_mappings_ = [&]() {
        if (impl_->gl_buffer_mappings_.empty() || !impl_->gl_buffer_subdata_fn_)
            return;
        // Snapshot (buffer id + mapping) under mu; each push re-checks
        // liveness under mu first: a concurrent UNMAP/DELETE erases and
        // hands the bounce range back for reuse, and pushing from reused
        // pages would upload another allocation's bytes to the GPU.
        std::vector<std::pair<uint32_t, GraphicThunkImpl::BufferMapping>> snaps;
        {
            std::lock_guard<std::mutex> g(impl_->mu);
            for (const auto& kv : impl_->gl_buffer_mappings_) {
                const auto& m = kv.second;
                if ((m.access & (kGLMapPersistentBit | kGLMapCoherentBit |
                                 kGLMapWriteBit)) ==
                    (kGLMapPersistentBit | kGLMapCoherentBit | kGLMapWriteBit)) {
                    snaps.emplace_back(kv.first, m);
                }
            }
        }
        if (snaps.empty()) return;
        using SubFn = void (*)(uint32_t, uint64_t, uint64_t, const void*);
        for (const auto& [buffer, m] : snaps) {
            {
                std::lock_guard<std::mutex> g(impl_->mu);
                auto it = impl_->gl_buffer_mappings_.find(buffer);
                if (it == impl_->gl_buffer_mappings_.end() ||
                    it->second.bounce != m.bounce || it->second.size != m.size)
                    continue;  // unmapped/freed/re-mapped since snapshot
            }
            uint8_t* hp = impl_->mem->guest_to_host_ptr(m.bounce);
            if (hp) {
                reinterpret_cast<SubFn>(impl_->gl_buffer_subdata_fn_)(
                    m.target, m.offset, m.size, hp);
            }
        }
    };
    if (entry.spec) {
        thunk::Policy mpol = entry.spec->policy;
        if (mpol == thunk::Policy::MAP_BUFFER) {
            bool is_range = (entry.name == "glMapBufferRange");
            uint32_t target = static_cast<uint32_t>(args[0]);
            uint32_t access = static_cast<uint32_t>(is_range ? args[3] : args[1]);
            if (!is_range) {
                // glMapBuffer takes an ACCESS ENUM, not a bitfield:
                // GL_READ_ONLY=0x88B8 / GL_WRITE_ONLY=0x88B9 /
                // GL_READ_WRITE=0x88BA. None of their bits overlap the
                // GL_MAP_*_BIT masks (e.g. 0x88B9 & GL_MAP_WRITE_BIT==0),
                // so without this normalization the writeback-on-unmap
                // check below silently dropped every glMapBuffer write.
                if (access == 0x88B8)      access = kGLMapReadBit;
                else if (access == 0x88B9) access = kGLMapWriteBit;
                else if (access == 0x88BA) access = kGLMapReadBit | kGLMapWriteBit;
            }
            uint64_t offset = is_range ? args[1] : 0;
            uint64_t length = is_range ? args[2] : 0;
            cpu.regs[0] = 0;
            if (!impl_->mem || !impl_->gl_state_tracker_ ||
                !impl_->gl_get_buffer_parameteriv_fn_) {
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] %s: unavailable (mem/tracker/host fns)\n",
                            entry.name.c_str());
                return 0;
            }
            uint32_t buffer = impl_->gl_state_tracker_->buffer_binding(target);
            if (buffer == 0) {
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] %s: no buffer bound to target 0x%x\n",
                            entry.name.c_str(), target);
                return 0;  // GL: map of an unbound buffer → NULL
            }
            {
                // Reserve the buffer key with a PLACEHOLDER (bounce=0)
                // under the lock — this is what actually makes the
                // double-map check atomic. The old code only re-checked
                // here and inserted much later (after mmap_alloc + host
                // GL queries), so two threads could both pass the check,
                // both allocate a bounce, and the second insert would
                // silently orphan the first. Holders see bounce=0 and
                // skip writeback/untrack until the real mapping lands.
                std::lock_guard<std::mutex> g(impl_->mu);
                if (impl_->gl_buffer_mappings_.count(buffer)) {
                    if (dbg().thunk_trace)
                        fprintf(stderr, "[thunk] %s: buffer %u already mapped\n",
                                entry.name.c_str(), buffer);
                    return 0;  // GL: mapping an already-mapped buffer → NULL
                }
                impl_->gl_buffer_mappings_[buffer] =
                    GraphicThunkImpl::BufferMapping{0, 0, 0, target, access};
            }
            if (!is_range) {
                // Whole-buffer map: query the host for the buffer size.
                int32_t size = 0;
                using ParamFn = void (*)(uint32_t, uint32_t, int32_t*);
                reinterpret_cast<ParamFn>(impl_->gl_get_buffer_parameteriv_fn_)(
                    target, kGLBufferSize, &size);
                length = (size > 0) ? static_cast<uint64_t>(size) : 0;
            }
            // Slow-path failure cleanup: drop OUR placeholder (and only
            // the placeholder — bounce==0 — so a real mapping installed
            // by a racing map after our unmap can't be nuked) or the
            // buffer stays "mapped" forever.
            auto drop_placeholder_ = [&]() {
                std::lock_guard<std::mutex> g(impl_->mu);
                auto it = impl_->gl_buffer_mappings_.find(buffer);
                if (it != impl_->gl_buffer_mappings_.end() && it->second.bounce == 0)
                    impl_->gl_buffer_mappings_.erase(it);
            };
            if (length == 0) {
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] %s: buffer %u has zero size\n",
                            entry.name.c_str(), buffer);
                drop_placeholder_();
                return 0;
            }
            uint64_t bounce = impl_->mem->mmap_alloc(length);
            if (bounce == 0) {
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] %s: mmap_alloc(%llu) failed\n",
                            entry.name.c_str(),
                            static_cast<unsigned long long>(length));
                drop_placeholder_();
                return 0;
            }
            // Seed the bounce from the host buffer if the guest may read it.
            if ((access & kGLMapReadBit) &&
                !(access & (kGLMapInvalidateRangeBit | kGLMapInvalidateBufferBit)) &&
                impl_->gl_get_buffer_subdata_fn_) {
                uint8_t* host_ptr = impl_->mem->guest_to_host_ptr(bounce);
                if (host_ptr) {
                    using GetSubFn = void (*)(uint32_t, uint64_t, uint64_t, void*);
                    reinterpret_cast<GetSubFn>(impl_->gl_get_buffer_subdata_fn_)(
                        target, offset, length, host_ptr);
                }
            }
            {
                std::lock_guard<std::mutex> g(impl_->mu);
                impl_->gl_buffer_mappings_[buffer] =
                    GraphicThunkImpl::BufferMapping{bounce, length, offset,
                                                    target, access};
            }
            cpu.regs[0] = bounce;
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] %s: buffer=%u target=0x%x off=%llu "
                        "len=%llu acc=0x%x → bounce=0x%llx\n",
                        entry.name.c_str(), buffer, target,
                        static_cast<unsigned long long>(offset),
                        static_cast<unsigned long long>(length), access,
                        static_cast<unsigned long long>(bounce));
            }
            return 0;
        }
        if (mpol == thunk::Policy::UNMAP_BUFFER) {
            uint32_t target = static_cast<uint32_t>(args[0]);
            if (impl_->mem && impl_->gl_state_tracker_ && impl_->gl_buffer_subdata_fn_) {
                uint32_t buffer = impl_->gl_state_tracker_->buffer_binding(target);
                GraphicThunkImpl::BufferMapping m{};
                bool found = false;
                bool persistent = false;
                {
                    std::lock_guard<std::mutex> g(impl_->mu);
                    auto it = impl_->gl_buffer_mappings_.find(buffer);
                    if (it != impl_->gl_buffer_mappings_.end()) {
                        m = it->second;
                        // PCWFC: a persistent mapping stays valid after
                        // glUnmapBuffer (GL_ARB_buffer_storage semantics) —
                        // keep the bounce + mapping alive so per-frame
                        // writes through the returned pointer still land.
                        persistent = (m.access & kGLMapPersistentBit) != 0;
                        if (!persistent) impl_->gl_buffer_mappings_.erase(it);
                        found = true;
                    }
                }
                if (found && (m.access & kGLMapWriteBit)) {
                    uint8_t* host_ptr = impl_->mem->guest_to_host_ptr(m.bounce);
                    if (host_ptr) {
                        using SubFn = void (*)(uint32_t, uint64_t, uint64_t, const void*);
                        reinterpret_cast<SubFn>(impl_->gl_buffer_subdata_fn_)(
                            m.target, m.offset, m.size, host_ptr);
                    }
                }
                if (found && !persistent && m.bounce) impl_->mem->untrack_allocation(m.bounce, m.size);
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] glUnmapBuffer: buffer=%u target=0x%x "
                            "%s%s\n", buffer, target, found ? "ok" : "(not mapped)",
                            persistent ? " (persistent, kept)" : "");
            }
            cpu.regs[0] = 1;  // GL_TRUE
            return 0;
        }
        if (mpol == thunk::Policy::FLUSH_BUFFER) {
            // glFlushMappedBufferRange(target, offset, length): push the
            // guest-written range to the host now (GL_MAP_FLUSH_EXPLICIT /
            // persistent+coherent best-effort). The bounce holds
            // buffer[offset..offset+size), so flush its [offset..+length).
            uint32_t target = static_cast<uint32_t>(args[0]);
            uint64_t offset = args[1];
            uint64_t length = args[2];
            if (impl_->mem && impl_->gl_state_tracker_ && impl_->gl_buffer_subdata_fn_) {
                uint32_t buffer = impl_->gl_state_tracker_->buffer_binding(target);
                GraphicThunkImpl::BufferMapping m{};
                bool found = false;
                {
                    std::lock_guard<std::mutex> g(impl_->mu);
                    auto it = impl_->gl_buffer_mappings_.find(buffer);
                    if (it != impl_->gl_buffer_mappings_.end()) {
                        m = it->second;
                        found = true;
                    }
                }
                if (found && (m.access & kGLMapWriteBit)) {
                    // The bounce holds buffer[offset..offset+size); map the
                    // flush request into it and clamp to the mapped range.
                    uint64_t rel = (offset >= m.offset) ? offset - m.offset : 0;
                    uint64_t n = length;
                    if (rel + n > m.size) n = (rel < m.size) ? m.size - rel : 0;
                    uint8_t* host_ptr = impl_->mem->guest_to_host_ptr(m.bounce + rel);
                    if (host_ptr && n > 0) {
                        using SubFn = void (*)(uint32_t, uint64_t, uint64_t, const void*);
                        reinterpret_cast<SubFn>(impl_->gl_buffer_subdata_fn_)(
                            m.target, offset, n, host_ptr);
                    }
                }
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] glFlushMappedBufferRange: buffer=%u "
                            "target=0x%x off=%llu len=%llu %s\n",
                            buffer, target,
                            static_cast<unsigned long long>(offset),
                            static_cast<unsigned long long>(length),
                            found ? "ok" : "(not mapped)");
            }
            cpu.regs[0] = 0;
            return 0;
        }
    }

    // GetProcAddress(name): return guest trampoline for a registered
    // GL/EGL/SDL symbol, or 0 if unknown. Games resolve most GL via this.
    if (entry.flags & THUNK_GET_PROC) {
        char namebuf[256];
        const char* name = nullptr;
        if (args[0] && impl_->mem) {
            uint8_t* hp = impl_->mem->guest_to_host_ptr(args[0]);
            if (hp) {
                // Window alias reads can't fault (full reservation), but
                // an unterminated guest pointer would run into garbage:
                // accept it only with a NUL inside 255 bytes, else fall
                // through to the bounded copy below.
                size_t n = 0;
                while (n < sizeof(namebuf) - 1 && hp[n]) n++;
                if (hp[n] == 0) {
                    name = reinterpret_cast<const char*>(hp);
                }
            }
            if (!name) {
                size_t n = 0;
                for (; n + 1 < sizeof(namebuf); n++) {
                    uint8_t c = 0;
                    try { impl_->mem->read(args[0] + n, &c, 1); }
                    catch (...) { break; }
                    namebuf[n] = static_cast<char>(c);
                    if (c == 0) break;
                }
                namebuf[sizeof(namebuf) - 1] = 0;
                name = namebuf;
            }
        }
        uint64_t found = 0;
        if (name && name[0]) {
            // ARB-suffixed aliases (glBindBufferARB, glActiveTextureARB,
            // …): games built against GL_ARB_* extensions fetch the
            // suffixed name while the table registers the core name.
            // Strip a trailing "ARB" and retry before giving up.
            char corebuf[256];
            const char* try_names[2] = { name, nullptr };
            size_t len = strlen(name);
            if (len > 3 && len < sizeof(corebuf) &&
                    !strcmp(name + len - 3, "ARB")) {
                memcpy(corebuf, name, len - 3);
                corebuf[len - 3] = 0;
                try_names[1] = corebuf;
            }
            for (int ti = 0; ti < 2 && !found; ti++) {
                if (!try_names[ti]) break;
                for (const auto& lib : impl_->libs_) {
                    for (const auto& e : lib.entries) {
                        if (e.name == try_names[ti]) {
                            found = e.guest_addr;
                            break;
                        }
                    }
                    if (found) break;
                }
            }
        }
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] GetProcAddress('%s') → 0x%llx\n",
                    name ? name : "(null)",
                    static_cast<unsigned long long>(found));
        }
        cpu.regs[0] = found;
        return 0;
    }

    // SizeKinds whose pointer arg is a pure HOST INPUT (upload/read):
    // the host never writes through that bounce, so marking it
    // need_wb would copy stale bounce bytes back over live guest
    // memory after the call. Download/query kinds (READPIXELS,
    // STIPPLE, X_*) still write back normally.
    auto input_only_size = [](thunk::SizeKind sk) {
        switch (sk) {
            case thunk::SizeKind::ARG1: case thunk::SizeKind::ARG2:
            case thunk::SizeKind::ARG6:
            case thunk::SizeKind::TEX2D: case thunk::SizeKind::TEXSUB:
            case thunk::SizeKind::TEX3D:
            case thunk::SizeKind::TEXIMAGE1D: case thunk::SizeKind::TEXSUBIMAGE1D:
            case thunk::SizeKind::TEXSUBIMAGE3D:
            case thunk::SizeKind::DRAWPIXELS: case thunk::SizeKind::BITMAP:
            case thunk::SizeKind::QUEUEAUDIO: case thunk::SizeKind::PITCH_H:
            case thunk::SizeKind::VK_REGIONS:
                return true;
            default:
                return false;
        }
    };

    auto translate_ptr = [&](uint64_t& a, int idx,
                             std::vector<uint8_t>* bounce,
                             uint64_t* guest_orig,
                             bool* need_wb) {
        if (a == 0 || !impl_->mem) return;
        uint8_t* host_ptr = impl_->mem->guest_to_host_ptr(a);
        if (host_ptr) {
            a = reinterpret_cast<uint64_t>(host_ptr);
            return;
        }
        // High-stack / sparse-page pointer: bounce through a host buffer.
        // Default 64 KiB covers modest textures/VBO uploads; the SIZE
        // column of the spec overrides it where the exact size is known.
        size_t kBounce = 65536;
#if defined(BIFROST_THUNK_HAVE_SDL2)
        // SDL event/out-int buffers are small fixed ABI objects. Reading
        // a generic 64 KiB bounce across a callback stack's guard page
        // fails and substitutes zeros, silently losing nested input.
        if (idx == 0 && (entry.name == "SDL_PushEvent" ||
                         entry.name == "SDL_PollEvent" ||
                         entry.name == "SDL_WaitEvent" ||
                         entry.name == "SDL_WaitEventTimeout"))
            kBounce = sizeof(SDL_Event);
        if ((entry.name == "SDL_GetMouseState" ||
             entry.name == "SDL_GetRelativeMouseState") && idx < 2)
            kBounce = sizeof(int);
#endif
#if defined(BIFROST_THUNK_HAVE_SDL2)
        if ((entry.name == "SDL_GetWindowPosition" ||
             entry.name == "SDL_GetWindowSize") && idx >= 1)
            kBounce = sizeof(int);
        if (entry.name == "SDL_GetDisplayDPI" && idx >= 1)
            kBounce = sizeof(float);
        if (entry.name == "SDL_GetWindowDisplayMode" && idx == 1)
            kBounce = sizeof(SDL_DisplayMode);
        if (entry.name == "SDL_SetTextInputRect" && idx == 0)
            kBounce = sizeof(SDL_Rect);
        if (entry.name == "SDL_GetWindowGammaRamp" && idx >= 1)
            kBounce = 256 * sizeof(uint16_t);
#endif
        // Padded row extent the host driver accesses on pixel-transfer
        // bounces (rows strided to tracked UNPACK/PACK alignment).
        uint64_t pad_extent = 0;
        auto pixel_extent_ = [&](bool upload, uint64_t h,
                                 uint64_t row_bytes) -> uint64_t {
            int align = 4;
            if (impl_->gl_state_tracker_) {
                align = upload
                    ? impl_->gl_state_tracker_->pixel_store_unpack_alignment()
                    : impl_->gl_state_tracker_->pixel_store_pack_alignment();
            }
            uint64_t stride = (row_bytes + align - 1) & ~(uint64_t)(align - 1);
            return h ? (h - 1) * stride + row_bytes : 0;
        };
        // Bytes per pixel for a GL format/type pair. Packed pixel types
        // have fixed sizes independent of format component count.
        // Unknown combos conservatively default to one byte/pixel.
        auto pixel_bps_ = [](uint64_t fmt, uint64_t type) -> uint64_t {
            uint64_t channels = 1;
            switch (fmt) {
                case 0x1907: case 0x80E0: channels = 3; break;  // GL_RGB / GL_BGR
                case 0x1908: case 0x80E1: channels = 4; break;  // GL_RGBA / GL_BGRA
                case 0x190A: case 0x8227: case 0x8228:
                case 0x822B: case 0x822C: channels = 2; break;  // LA / RG
                case 0x84F9: channels = 2; break;               // GL_DEPTH_STENCIL
                case 0x8229: case 0x822A: case 0x8D94:
                case 0x1903: case 0x1904: case 0x1905:
                case 0x1906: case 0x1909: case 0x1902:
                    channels = 1; break;                         // RED / ALPHA / DEPTH
                default: break;                                  // 1 (GL_RED/GL_ALPHA/...)
            }
            switch (type) {
                case 0x8032: case 0x8362: return 1;              // packed 3-3-2
                case 0x8363: case 0x8364:                       // packed 5-6-5
                case 0x8033: case 0x8365:                       // packed 4-4-4-4
                case 0x8034: case 0x8366: return 2;             // packed 5-5-5-1
                case 0x8035: case 0x8367:                       // packed 8-8-8-8
                case 0x8036: case 0x8368:                       // packed 10-10-10-2
                case 0x8C3B: case 0x8C3E: case 0x84FA: return 4;// packed 32-bit
                case 0x8DAD: return 8;                          // packed depth/stencil
                case 0x1400: case 0x1401: return channels;      // BYTE/UBYTE
                case 0x1402: case 0x1403: case 0x140B:
                    return channels * 2;                        // SHORT/USHORT/HALF
                case 0x1404: case 0x1405: case 0x1406: case 0x140C:
                    return channels * 4;                        // INT/UINT/FLOAT/FIXED
                default: return channels;
            }
        };
        const thunk::SizeKind sk = entry.spec ? entry.spec->size
                                              : thunk::SizeKind::NONE;
        switch (sk) {
        case thunk::SizeKind::ARG1:
            // glBufferData(target, size, data, usage): data is arg2.
            if (idx == 2) {
                uint64_t sz = args[1];
                if (sz > 0 && sz < (16ull << 20)) kBounce = static_cast<size_t>(sz);
            }
            break;
        case thunk::SizeKind::ARG2:
            // glBufferSubData(target, offset, size, data): data is arg3.
            if (idx == 3) {
                uint64_t sz = args[2];
                if (sz > 0 && sz < (16ull << 20)) kBounce = static_cast<size_t>(sz);
            }
            break;
        case thunk::SizeKind::ARG6:
            // glCompressedTexImage2D(target, level, internalformat, width,
            // height, border, imageSize, data): data is arg7, sized by
            // arg6 (imageSize).
            if (idx == 7) {
                uint64_t sz = args[6];
                if (sz > 0 && sz < (16ull << 20)) kBounce = static_cast<size_t>(sz);
            }
            break;
        case thunk::SizeKind::TEX2D:
            // glTexImage2D(target, level, internalformat, width, height,
            // border, format, type, pixels): pixels is arg8. Width/height
            // are args 3/4.
            if (idx == 8) {
                uint64_t w = args[3], h = args[4];
                uint64_t fmt = args[6], type = args[7];
                const uint64_t bps = pixel_bps_(fmt, type);
                uint64_t sz = w * h * bps;
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, h,
                                               w * bps);
                }
            }
            break;
        case thunk::SizeKind::TEXSUB:
            // glTexSubImage2D(target, level, xoffset, yoffset, width,
            // height, format, type, pixels): pixels is arg8. Width/height
            // are args 4/5 (args 3 is yoffset, NOT width).
            if (idx == 8) {
                uint64_t w = args[4], h = args[5];
                uint64_t fmt = args[6], type = args[7];
                const uint64_t bps = pixel_bps_(fmt, type);
                uint64_t sz = w * h * bps;
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, h,
                                               w * bps);
                }
            }
            break;
        case thunk::SizeKind::TEX3D:
            // glTexImage3D(target, level, internalformat, width, height,
            // depth, border, format, type, pixels): pixels is arg9. Host
            // reads width*height*depth*channels*type_size — a font atlas
            // volume dwarfs the default 64 KiB bounce.
            if (idx == 9) {
                uint64_t w = args[3], h = args[4], d = args[5];
                uint64_t fmt = args[7], type = args[8];
                const uint64_t bps = pixel_bps_(fmt, type);
                uint64_t sz = w * h * d * bps;
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, h * d, w * bps);
                }
            }
            break;
        case thunk::SizeKind::DRAWPIXELS:
            // glDrawPixels(width, height, format, type, pixels): pixels is
            // arg4 (upload — host reads it).
            if (idx == 4) {
                uint64_t w = args[0], h = args[1];
                uint64_t sz = w * h * pixel_bps_(args[2], args[3]);
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, h,
                                               w * pixel_bps_(args[2], args[3]));
                }
            }
            break;
        case thunk::SizeKind::BITMAP:
            // glBitmap(w, h, xorig, yorig, xmove, ymove, bits): bits is
            // arg6; rows are (w+7)/8 bytes.
            if (idx == 6) {
                uint64_t w = args[0], h = args[1];
                uint64_t row = (w + 7) / 8;
                uint64_t sz = h * row;
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, h, row);
                }
            }
            break;
        case thunk::SizeKind::TEXIMAGE1D:
            // glTexImage1D(target, level, internalformat, width, border,
            // format, type, data): data is arg7, one row of width pixels.
            if (idx == 7) {
                uint64_t w = args[3];
                uint64_t sz = w * pixel_bps_(args[5], args[6]);
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, 1,
                                               w * pixel_bps_(args[5], args[6]));
                }
            }
            break;
        case thunk::SizeKind::TEXSUBIMAGE1D:
            // glTexSubImage1D(target, level, xoffset, width, format, type,
            // data): data is arg6, one row of width pixels.
            if (idx == 6) {
                uint64_t w = args[3];
                uint64_t sz = w * pixel_bps_(args[4], args[5]);
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, 1,
                                               w * pixel_bps_(args[4], args[5]));
                }
            }
            break;
        case thunk::SizeKind::TEXSUBIMAGE3D:
            // glTexSubImage3D(target, level, xo, yo, zo, w, h, d, format,
            // type, data): data is arg10.
            if (idx == 10) {
                uint64_t w = args[5], h = args[6], d = args[7];
                uint64_t bps = pixel_bps_(args[8], args[9]);
                uint64_t sz = w * h * d * bps;
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, h * d, w * bps);
                }
            }
            break;
        case thunk::SizeKind::STIPPLE:
            // glPolygonStipple/glGetPolygonStipple(mask): fixed 32×32-bit
            // image = 128 bytes (rows of 4 bytes match default alignment).
            kBounce = 128;
            break;
        case thunk::SizeKind::VK_REGIONS:
            // vkCmdBlitImage(cmd, src, srcLayout, dst, dstLayout,
            // regionCount, pRegions): pRegions is an array of
            // VkImageBlit2 (96 B each, no guest-pointer members — a
            // byte-exact bounce copy is a faithful deep copy).
            if (idx == 6) {
                uint64_t sz = args[5] * 96;
                if (sz > 0 && sz < (16ull << 20)) kBounce = static_cast<size_t>(sz);
            }
            break;
        case thunk::SizeKind::PITCH_H:
            // SDL_UpdateTexture: pixels buffer = pitch (args[3]) * height.
            if (idx == 2) {
                uint64_t h = 0;
                {
                    std::lock_guard<std::mutex> lk(impl_->state_mu);
                    auto it = impl_->sdl_tex_sizes_.find(args[0]);
                    if (it != impl_->sdl_tex_sizes_.end()) h = it->second.second;
                }
                if (h) {
                    uint64_t sz = args[3] * h;
                    if (sz > 0 && sz < (16ull << 20)) kBounce = static_cast<size_t>(sz);
                }
            }
            break;
        case thunk::SizeKind::READPIXELS:
            // glReadPixels(x, y, width, height, format, type, pixels):
            // host writes width*height*channels*type_size into the out-buffer.
            if (idx == 6) {
                uint64_t w = args[2], h = args[3];
                uint64_t fmt = args[4], type = args[5];
                const uint64_t bps = pixel_bps_(fmt, type);
                uint64_t sz = w * h * bps;
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(false, h,
                                               w * bps);
                }
            }
            break;
        case thunk::SizeKind::QUEUEAUDIO:
            // SDL_QueueAudio(dev, data, len): host reads exactly len bytes.
            if (idx == 1) {
                uint64_t sz = args[2];
                if (sz > 0 && sz < (64ull << 20)) kBounce = static_cast<size_t>(sz);
            }
            break;
        default:
            break;
        }
        if (entry.name == "glTexImage2D" && dbg().thunk_trace) {
            static int tex_trace_n = 0;
            if (tex_trace_n++ < 80) {
                fprintf(stderr,
                        "[thunk] glTexImage2D full: target=0x%llx level=%llu "
                        "internal=0x%llx %llux%llu border=%llu format=0x%llx "
                        "type=0x%llx bytes=%zu\n",
                        (unsigned long long)args[0],
                        (unsigned long long)args[1],
                        (unsigned long long)args[2],
                        (unsigned long long)args[3],
                        (unsigned long long)args[4],
                        (unsigned long long)args[5],
                        (unsigned long long)args[6],
                        (unsigned long long)args[7], kBounce);
            }
        }
        // Copy the full pixel-store extent. Reserving capacity alone is not
        // enough: GL reads/writes row padding too, and uploads need those
        // guest bytes at the offsets the host driver will consume.
        if (pad_extent > static_cast<uint64_t>(kBounce) &&
            pad_extent < (64ull << 20))
            kBounce = static_cast<size_t>(pad_extent);
        bounce->resize(kBounce);
        try {
            impl_->mem->read(a, bounce->data(), kBounce);
        } catch (...) {
            // Keep the full padded extent available to the host driver.
            std::fill(bounce->begin(), bounce->end(), 0);
        }
        *guest_orig = a;
        *need_wb = !input_only_size(sk);
        a = reinterpret_cast<uint64_t>(bounce->data());
        (void)idx;
    };

    std::vector<uint8_t> bounce_bufs[kMaxArgs];
    uint64_t bounce_guest[kMaxArgs] = {0};
    bool bounce_wb[kMaxArgs] = {false};

    // VA_PTR/EL_PTR symbols own their pointer args (the binding-aware block
    // below translates them ONLY when no VBO is bound — with a buffer bound
    // the arg is a byte offset into that buffer and must pass through raw).
    // EL_PTR_ARRAY/INDIRECT_PTR own theirs the same way (nested/indirect
    // arms below). The generic loop must NOT touch them: translating offset
    // 4/12 as guest addresses turned them into host alias pointers that
    // host GL then reinterpreted as ~2^47 byte offsets (neverball's menu
    // GUI drew nothing). Also prevents a double translation when
    // binding==0.
    const bool va_ptr_owned =
        entry.spec && (entry.spec->policy == thunk::Policy::VA_PTR ||
                       entry.spec->policy == thunk::Policy::EL_PTR ||
                       entry.spec->policy == thunk::Policy::EL_PTR_ARRAY ||
                       entry.spec->policy == thunk::Policy::INDIRECT_PTR);
    // Pixel pack/unpack buffer arguments are byte offsets, not CPU pointers.
    // Leave them untouched so host GL can interpret them relative to the
    // currently bound PBO. This covers the sized paths and other registered
    // texture transfer calls whose signature still carries a pointer.
    auto starts_with = [&](const char* prefix) {
        return entry.name.compare(0, std::strlen(prefix), prefix) == 0;
    };
    const bool pixel_unpack_call =
        starts_with("glTexImage") || starts_with("glTexSubImage") ||
        starts_with("glCompressedTexImage") ||
        starts_with("glCompressedTexSubImage") ||
        starts_with("glTextureSubImage") ||
        starts_with("glCompressedTextureSubImage") ||
        entry.name == "glDrawPixels" || entry.name == "glBitmap" ||
        starts_with("glColorTable");
    const bool pixel_pack_call =
        entry.name == "glReadPixels" || starts_with("glGetTexImage") ||
        starts_with("glGetCompressedTexImage") ||
        starts_with("glGetnTexImage") || entry.name == "glReadnPixels" ||
        starts_with("glGetTextureImage") ||
        starts_with("glGetCompressedTextureImage");
    bool pixel_data_is_pbo_offset = false;
    if (impl_->gl_state_tracker_) {
        constexpr uint32_t pixel_pack_buffer_target = 0x88EB;
        constexpr uint32_t pixel_unpack_buffer_target = 0x88EC;
        if (pixel_unpack_call)
            pixel_data_is_pbo_offset =
                impl_->gl_state_tracker_->buffer_binding(pixel_unpack_buffer_target) != 0;
        else if (pixel_pack_call)
            pixel_data_is_pbo_offset =
                impl_->gl_state_tracker_->buffer_binding(pixel_pack_buffer_target) != 0;
    }
    // ── DELETE_TRACK: mirror deletes into the state tracker ──────
    // Deleting a bound object unbinds it; without mirroring, name reuse
    // would observe a stale "already bound" and elide a real bind.
    // Rare calls (resource teardown, not per-frame). Falls through.
    if (entry.spec && entry.spec->policy == thunk::Policy::DELETE_TRACK &&
        impl_->gl_state_tracker_ && impl_->mem) {
        auto* trk = impl_->gl_state_tracker_.get();
        if (entry.name == "glDeleteTextures") {
            uint64_t n = args[0] & 0xFFFFFFFFu;
            if (n > 0 && n <= 4096 && args[1]) {
                std::vector<uint32_t> ids(static_cast<size_t>(n));
                try {
                    impl_->mem->read(args[1], ids.data(), n * 4);
                    trk->unbind_textures(ids.data(), static_cast<size_t>(n));
                } catch (...) { trk->clear_texture_bindings(); }
            } else if (n > 0) {
                trk->clear_texture_bindings();
            }
        } else if (entry.name == "glDeleteProgram") {
            trk->unbind_program(static_cast<uint32_t>(args[0]));
        } else if (entry.name == "glDeleteVertexArrays") {
            uint64_t n = args[0] & 0xFFFFFFFFu;
            if (n > 0 && n <= 4096 && args[1]) {
                std::vector<uint32_t> ids(static_cast<size_t>(n));
                try {
                    impl_->mem->read(args[1], ids.data(), n * 4);
                    trk->unbind_vaos(ids.data(), static_cast<size_t>(n));
                } catch (...) { trk->mark_vao_unknown(); }
            } else if (n > 0) {
                trk->mark_vao_unknown();
            }
        }
        // Fall through to the generic path for the real call.
    }
    // ── ELIDE_BIND: skip redundant binding-point writes ────────────
    // The game rebinds already-bound objects per draw (measured ~74%
    // state-redundant). Eliding saves the whole ~75ns round trip. ONLY
    // functions whose effect is exactly "binding point = name" with no
    // capture semantics (buffer binds excluded: ARRAY/element state is
    // captured into VAOs at attrib-pointer time). All return void.
    // BIFROST_NO_ELIDE=1 disables. Single-context assumption (same as
    // the tracker itself).
    static const bool no_elide_ = getenv("BIFROST_NO_ELIDE") != nullptr;
    if (!no_elide_ && entry.spec &&
        entry.spec->policy == thunk::Policy::ELIDE_BIND &&
        impl_->gl_state_tracker_) {
        auto* trk = impl_->gl_state_tracker_.get();
        bool elided = false;
        if (entry.name == "glUseProgram") {
            elided = (trk->current_program() == static_cast<uint32_t>(args[0]));
        } else if (entry.name == "glActiveTexture") {
            elided = (trk->active_texture() == static_cast<uint32_t>(args[0]));
        } else if (entry.name == "glBindTexture") {
            elided = (trk->texture_binding(static_cast<uint32_t>(args[0])) ==
                      static_cast<uint32_t>(args[1]));
        } else if (entry.name == "glBindVertexArray") {
            bool known = false;
            uint32_t bound = trk->vao_binding(known);
            elided = (known && bound == static_cast<uint32_t>(args[0]));
        } else if (entry.name == "glBindBuffer") {
            // ARRAY_BUFFER is global state; ELEMENT_ARRAY_BUFFER is VAO
            // state (per-VAO capture — a VAO switch restores it with no
            // BindBuffer call). Other/indexed targets never elide: the
            // general map is last-index-wins, not exact per index.
            uint32_t target = static_cast<uint32_t>(args[0]);
            uint32_t buf = static_cast<uint32_t>(args[1]);
            if (target == 0x8892) {  // GL_ARRAY_BUFFER
                elided = (trk->array_buffer_binding() == buf);
            } else if (target == 0x8893) {  // GL_ELEMENT_ARRAY_BUFFER
                bool known = false;
                uint32_t cur = trk->vao_element_binding(known);
                elided = (known && cur == buf);
            }
        }
        if (elided) {
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] elide %s (redundant)\n",
                        entry.name.c_str());
            }
            cpu.regs[0] = 0;
            return 0;
        }
    }
    if (entry.pointer_args && impl_->mem && !va_ptr_owned &&
        !pixel_data_is_pbo_offset) {
        for (int i = 0; i < kMaxArgs; i++) {
            if (entry.pointer_args & (1u << i)) {
                translate_ptr(args[i], i, &bounce_bufs[i],
                              &bounce_guest[i], &bounce_wb[i]);
            }
        }
    }

    // ── SDL_JoystickGetGUIDString — guid BY VALUE, out-buffer bounce ──
    // Guest AAPCS64: x0/x1 = 16-byte SDL_JoystickGUID (by value), x2 =
    // char* pszGUID (OUT), x3 = cbGUID. The pointer-args loop above already
    // translated args[2]: either a bounce (write back below) or a direct
    // host alias (host wrote into guest memory in place, nothing to copy).
    // Must run AFTER the pointer-args loop so args[2] is host-addressable.
    if (entry.spec && entry.spec->policy == thunk::Policy::JOY_GUID_STR) {
        if (entry.host_fn && args[2] != 0) {
            uint64_t lo = args[0], hi = args[1];
            int size = static_cast<int>(args[3]);
            if (size < 0) size = 0;
            using Fn = void (*)(uint64_t, uint64_t, char*, int);
            reinterpret_cast<Fn>(entry.host_fn)(
                lo, hi, reinterpret_cast<char*>(args[2]), size);
            if (impl_->mem && bounce_guest[2] && bounce_wb[2]) {
                // Clamp the writeback to the guest's buffer capacity
                // (cbGUID) so we never overrun pszGUID[cbGUID].
                size_t wb = static_cast<size_t>(size);
                if (wb > bounce_bufs[2].size()) wb = bounce_bufs[2].size();
                impl_->mem->write(bounce_guest[2], bounce_bufs[2].data(), wb);
            }
        }
        cpu.regs[0] = 0;
        return 0;
    }

    // ── glfwCreateWindow HiDPI compensation ───────────────────────────
    // 1.5.4-alpha: the game requests a LOGICAL window size (1280x720),
    // but on HiDPI sessions (KDE Scale=2, Wayland/X11) host GLFW makes
    // the PHYSICAL framebuffer scale * the request (2560x1440), and
    // window.c then adopts the framebuffer size as window.size — the
    // game runs at double resolution and the HUD/matrices are laid out
    // for a fullscreen-scale window. Resize the host window by
    // 1/content_scale so glfwGetFramebufferSize returns exactly the
    // requested size. args[2] (title) was translated to a host pointer
    // by the pointer-args loop above.
    if (entry.spec && entry.spec->policy == thunk::Policy::GLFW_CREATE) {
        if (!impl_->glfw_get_window_content_scale_fn_ ||
            !impl_->glfw_set_window_size_fn_ || !entry.host_fn) {
            if (dbg().thunk_trace)
                fprintf(stderr, "[thunk] glfwCreateWindow: host fns unavailable\n");
            cpu.regs[0] = 0;
            return 0;
        }
        using CreateFn = uint64_t (*)(int, int, const char*, uint64_t, uint64_t);
        uint64_t win = reinterpret_cast<CreateFn>(entry.host_fn)(
            static_cast<int>(args[0]), static_cast<int>(args[1]),
            reinterpret_cast<const char*>(args[2]), args[3], args[4]);
        cpu.regs[0] = win;
        window_stats::api(entry.name,args,win);
        if (win) {
            float xs = 1.0f, ys = 1.0f;
            using ScaleFn = void (*)(uint64_t, float*, float*);
            reinterpret_cast<ScaleFn>(impl_->glfw_get_window_content_scale_fn_)(
                win, &xs, &ys);
            if (xs > 1.01f || ys > 1.01f) {
                int nw = static_cast<int>(static_cast<int64_t>(args[0]) / xs + 0.5f);
                int nh = static_cast<int>(static_cast<int64_t>(args[1]) / ys + 0.5f);
                using SizeFn = void (*)(uint64_t, int, int);
                reinterpret_cast<SizeFn>(impl_->glfw_set_window_size_fn_)(win, nw, nh);
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] glfwCreateWindow %llux%llu -> resize "
                            "%dx%d (scale %.2f)\n",
                            static_cast<unsigned long long>(args[0]),
                            static_cast<unsigned long long>(args[1]), nw, nh, xs);
            } else if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] glfwCreateWindow %llux%llu (scale %.2f)\n",
                        static_cast<unsigned long long>(args[0]),
                        static_cast<unsigned long long>(args[1]), xs);
            }
        }
        return 0;
    }

    // VA_PTR/EL_PTR: these take a *byte offset* into the bound buffer (must
    // NOT be translated) when a buffer is bound, but a real guest pointer
    // for client-side vertex / index arrays (must be translated).
    // GLStateTracker knows the binding; the policy comes from the spec.
    if (impl_->gl_state_tracker_ && entry.spec) {
        if (entry.spec->policy == thunk::Policy::VA_PTR) {
            // Pointer-arg position differs per function:
            //   glVertexAttribPointer*(index,...,stride,ptr) -> arg5
            //   glVertexPointer(size,type,stride,ptr)        -> arg3
            //   glColorPointer(size,size,type,stride,ptr)    -> arg4
            //   glTexCoordPointer(size,type,stride,ptr)      -> arg1
            //   glNormalPointer(type,stride,ptr)             -> arg2
            int pi = 5;
            const char* nm = entry.name.c_str();
            if (!strcmp(nm, "glVertexAttribIPointer") || !strcmp(nm, "glVertexAttribLPointer"))
                pi = 4;  // these variants omit the normalized argument
            if (!strcmp(nm, "glVertexPointer") || !strcmp(nm, "glColorPointer"))
                pi = 3;                               // (size,type,stride,ptr)
            else if (!strcmp(nm, "glTexCoordPointer")) pi = 3;  // (size,type,stride,ptr)
            else if (!strcmp(nm, "glNormalPointer"))   pi = 2;  // (type,stride,ptr)
            static const bool vaptr_dbg = getenv("BIFROST_GUI_DBG");
            if (vaptr_dbg && (!strcmp(nm, "glVertexPointer") ||
                              !strcmp(nm, "glTexCoordPointer")))
                fprintf(stderr, "[vaptr] %s binding=%u arg=%llx -> %s\n",
                        nm, impl_->gl_state_tracker_->array_buffer_binding(),
                        (unsigned long long)args[pi],
                        impl_->gl_state_tracker_->array_buffer_binding() ?
                            "OFFSET" : "translate");
            if (impl_->gl_state_tracker_->array_buffer_binding() == 0 &&
                args[pi] != 0) {
                translate_ptr(args[pi], pi, &bounce_bufs[pi],
                              &bounce_guest[pi], &bounce_wb[pi]);
            }
        } else if (entry.spec->policy == thunk::Policy::EL_PTR) {
            // Index-array arg position differs per function:
            //   glDrawElements*/glDrawElementsInstanced -> arg3
            //   glDrawRangeElements(mode,start,end,count,type,indices) -> arg5
            //   glDrawRangeElementsBaseVertex(...,type,indices,base) -> arg5
            int pi = 3;
            if (entry.name == "glDrawRangeElements" ||
                entry.name == "glDrawRangeElementsBaseVertex") pi = 5;
            if (impl_->gl_state_tracker_->element_array_buffer_binding() == 0 &&
                args[pi] != 0) {
                translate_ptr(args[pi], pi, &bounce_bufs[pi],
                              &bounce_guest[pi], &bounce_wb[pi]);
            }
        } else if (entry.spec->policy == thunk::Policy::EL_PTR_ARRAY) {
            // glMultiDrawElements(mode, count[], type, indices[], primcount)
            // [+ basevertex[]]: bounce the outer arrays; each inner
            // indices element is a byte offset iff an EBO is bound, else
            // a translated client pointer sized by count[i]*index_size.
            // Input-only: no writeback of the index data.
            do {
                uint64_t n64 = args[4];
                if (n64 == 0 || n64 > 256 || !impl_->mem) break;
                uint32_t n = static_cast<uint32_t>(n64);
                uint32_t isz = 1;
                switch (static_cast<uint32_t>(args[2])) {
                case 0x1401: isz = 1; break;  // UNSIGNED_BYTE
                case 0x1403: isz = 2; break;  // UNSIGNED_SHORT
                case 0x1405: isz = 4; break;  // UNSIGNED_INT
                default: break;
                }
                bool ebo = impl_->gl_state_tracker_->
                    element_array_buffer_binding() != 0;
                // BaseVertex form has a 6th arg: int32 basevertex array.
                bool has_bv = entry.spec->args &&
                    strlen(entry.spec->args) == 6;
                // Frame-lived staging: the generic emission below reads
                // args[] synchronously, so thread-locals outlive the call.
                // (Vector move transfers element buffers without
                // reallocating, so inner data pointers stay stable.)
                static thread_local std::vector<uint8_t> live_counts;
                static thread_local std::vector<uint64_t> live_outer;
                static thread_local std::vector<std::vector<uint8_t>> live_in;
                static thread_local std::vector<uint8_t> live_bv;
                // Outer count array (int32[], always a guest pointer).
                live_counts.resize((size_t)n * 4);
                {
                    uint8_t* hp = impl_->mem->guest_to_host_ptr(args[1]);
                    if (hp) {
                        memcpy(live_counts.data(), hp, (size_t)n * 4);
                    } else {
                        try {
                            impl_->mem->read(args[1], live_counts.data(),
                                             (size_t)n * 4);
                        } catch (...) { break; }
                    }
                }
                const uint32_t* counts =
                    reinterpret_cast<const uint32_t*>(live_counts.data());
                // Outer indices array (guest ptr[]).
                std::vector<uint64_t> guest_ptrs(n, 0);
                try {
                    impl_->mem->read(args[3], guest_ptrs.data(),
                                     (size_t)n * 8);
                } catch (...) { break; }
                live_in.clear();
                live_in.resize(n);
                live_outer.assign(n, 0);
                bool ok = true;
                for (uint32_t i = 0; i < n; i++) {
                    if (ebo || guest_ptrs[i] == 0) {
                        live_outer[i] = guest_ptrs[i];  // offset or NULL
                        continue;
                    }
                    uint64_t bytes = (uint64_t)counts[i] * isz;
                    if (bytes == 0) {
                        live_outer[i] = 0;
                        continue;
                    }
                    uint8_t* hp = nullptr;
                    // Alias only if the WHOLE index buffer is readable.
                    hp = impl_->mem->guest_to_host_ptr(
                        guest_ptrs[i], Memory::GUEST_PROT_READ, bytes);
                    if (hp) {
                        // Direct-window alias: host reads guest memory
                        // directly, no bounce size cap needed.
                        live_outer[i] = reinterpret_cast<uint64_t>(hp);
                    } else {
                        // Bounce path only: cap to avoid a huge host alloc.
                        // Bogus counts fail closed (NULL + count would
                        // make the host over-read).
                        if (bytes > (16ull << 20)) {
                            cpu.regs[0] = 0;
                            return 0;
                        }
                        live_in[i].resize((size_t)bytes);
                        try {
                            impl_->mem->read(guest_ptrs[i],
                                             live_in[i].data(),
                                             (size_t)bytes);
                        } catch (...) { ok = false; break; }
                        live_outer[i] = reinterpret_cast<uint64_t>(
                            live_in[i].data());
                    }
                }
                if (!ok) break;
                args[1] = reinterpret_cast<uint64_t>(live_counts.data());
                args[3] = reinterpret_cast<uint64_t>(live_outer.data());
                if (has_bv) {
                    // Basevertex int32 array (always a guest pointer).
                    live_bv.resize((size_t)n * 4);
                    uint8_t* hp = impl_->mem->guest_to_host_ptr(args[5]);
                    if (hp) {
                        memcpy(live_bv.data(), hp, (size_t)n * 4);
                    } else {
                        try {
                            impl_->mem->read(args[5], live_bv.data(),
                                             (size_t)n * 4);
                        } catch (...) { break; }
                    }
                    args[5] = reinterpret_cast<uint64_t>(live_bv.data());
                }
            } while (0);
        } else if (entry.spec->policy == thunk::Policy::INDIRECT_PTR) {
            // glDrawArraysIndirect(mode, indirect) / glDrawElementsIndirect
            // (mode, type, indirect): 16/20-byte command struct. Offset
            // into DRAW_INDIRECT_BUFFER when bound, else a translated
            // client struct.
            do {
                bool is_elem = entry.name == "glDrawElementsIndirect";
                int pi = is_elem ? 2 : 1;
                size_t sz = is_elem ? 20 : 16;
                if (impl_->gl_state_tracker_->
                        draw_indirect_buffer_binding() != 0 ||
                    args[pi] == 0)
                    break;  // bound offset or NULL: pass through raw
                uint8_t* hp = impl_->mem ?
                    impl_->mem->guest_to_host_ptr(args[pi]) : nullptr;
                if (hp) {
                    args[pi] = reinterpret_cast<uint64_t>(hp);
                    break;
                }
                static thread_local std::vector<uint8_t> live_ind;
                live_ind.resize(sz);
                try {
                    impl_->mem->read(args[pi], live_ind.data(), sz);
                } catch (...) { break; }
                args[pi] = reinterpret_cast<uint64_t>(live_ind.data());
            } while (0);
        }
    }

    // glShaderSource(shader, count, const char* const* strings, const int* len)
    if (entry.flags & THUNK_SHADER_SOURCE) {
        int count = static_cast<int>(args[1]);
        if (count < 0) count = 0;
        if (count > 64) count = 64;
        const char* host_strs[64];
        std::vector<std::vector<uint8_t>> bounces;
        bounces.reserve(static_cast<size_t>(count));
        // args[2] already translated to host pointer to guest pointer array
        const uint64_t* guest_arr = reinterpret_cast<const uint64_t*>(args[2]);
        // args[3] (guest GLint* lengths, may be NULL) is already translated.
        const int* lens = args[3] ? reinterpret_cast<const int*>(args[3]) : nullptr;
        for (int i = 0; i < count; i++) {
            uint64_t gp = guest_arr ? guest_arr[i] : 0;
            if (gp == 0) { host_strs[i] = ""; continue; }
            uint8_t* hp = impl_->mem ? impl_->mem->guest_to_host_ptr(gp) : nullptr;
            if (hp) { host_strs[i] = reinterpret_cast<const char*>(hp); continue; }
            // Not readable as one host range (e.g. ends on a guard page): bounce it.
            size_t n = (lens && lens[i] > 0)
                           ? static_cast<size_t>(lens[i])
                           : (lens ? 0 : 65536);
            if (n == 0 && lens && lens[i] <= 0) n = 0;
            if (n == 0) {
                // Null-terminated string; scan for the terminator.
                constexpr size_t kMaxStr = 1 << 20;
                for (; n < kMaxStr; n++) {
                    uint8_t c = 0;
                    try { impl_->mem->read(gp + n, &c, 1); } catch (...) { break; }
                    if (c == 0) break;
                }
            }
            if (n > (16ull << 20)) n = 16ull << 20;
            bounces.emplace_back(n + 1, 0);
            try {
                impl_->mem->read(gp, bounces.back().data(), n);
            } catch (...) {
                bounces.back().assign(n + 1, 0);
            }
            host_strs[i] = reinterpret_cast<const char*>(bounces.back().data());
        }
        using Fn = void (*)(uint64_t, int, const char* const*, const int*);
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] shaderSource: shader=0x%llx count=%d "
                    "str0='%.60s' len0=%d\n",
                    static_cast<unsigned long long>(args[0]), count,
                    (host_strs[0] ? host_strs[0] : ""),
                    (lens ? lens[0] : -1));
        }
        reinterpret_cast<Fn>(entry.host_fn)(
            args[0], count, host_strs,
            args[3] ? reinterpret_cast<const int*>(args[3]) : nullptr);
        cpu.regs[0] = 0;
        return 0;
    }

    // glTransformFeedbackVaryings(program, count, const GLchar *const *varyings,
    // bufferMode): the varyings array is a nested array of C-string pointers
    // (exactly like glShaderSource's strings), so it needs the same per-string
    // translation/bounce. args[3] is bufferMode (a GLenum, NOT a length array),
    // so unlike shaderSource there is no lens vector — strings are read until
    // their NUL terminator.
    if (entry.flags & THUNK_TF_VARYINGS) {
        int count = static_cast<int>(args[1]);
        if (count < 0) count = 0;
        if (count > 64) count = 64;
        const char* host_strs[64];
        std::vector<std::vector<uint8_t>> bounces;
        bounces.reserve(static_cast<size_t>(count));
        // args[2] already translated to a host pointer to the guest pointer array.
        const uint64_t* guest_arr = reinterpret_cast<const uint64_t*>(args[2]);
        for (int i = 0; i < count; i++) {
            uint64_t gp = guest_arr ? guest_arr[i] : 0;
            if (gp == 0) { host_strs[i] = ""; continue; }
            uint8_t* hp = impl_->mem ? impl_->mem->guest_to_host_ptr(gp) : nullptr;
            if (hp) { host_strs[i] = reinterpret_cast<const char*>(hp); continue; }
            size_t n = 0;
            constexpr size_t kMaxStr = 1 << 20;
            for (; n < kMaxStr; n++) {
                uint8_t c = 0;
                try { impl_->mem->read(gp + n, &c, 1); } catch (...) { break; }
                if (c == 0) break;
            }
            if (n > (16ull << 20)) n = 16ull << 20;
            bounces.emplace_back(n + 1, 0);
            try {
                impl_->mem->read(gp, bounces.back().data(), n);
            } catch (...) {
                bounces.back().assign(n + 1, 0);
            }
            host_strs[i] = reinterpret_cast<const char*>(bounces.back().data());
        }
        using Fn = void (*)(uint64_t, int, const char* const*, uint32_t);
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] transformFeedbackVaryings: program=0x%llx "
                    "count=%d str0='%.60s' mode=0x%llx\n",
                    static_cast<unsigned long long>(args[0]), count,
                    (host_strs[0] ? host_strs[0] : ""),
                    static_cast<unsigned long long>(args[3]));
        }
        reinterpret_cast<Fn>(entry.host_fn)(
            args[0], count, host_strs, static_cast<uint32_t>(args[3]));
        cpu.regs[0] = 0;
        return 0;
    }

    if (dbg().thunk_trace) {
        fprintf(stderr, "[thunk] dispatch[T%lx]: %s (host_fn=%p) "
                "a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx "
                "a8=0x%llx ptrs=0x%x stack=%u\n",
                (unsigned long)pthread_self(),
                entry.name.c_str(), entry.host_fn,
                static_cast<unsigned long long>(args[0]),
                static_cast<unsigned long long>(args[1]),
                static_cast<unsigned long long>(args[2]),
                static_cast<unsigned long long>(args[3]),
                static_cast<unsigned long long>(args[8]),
                entry.pointer_args, entry.n_stack);
    }

    // 1.5.4-alpha: GL state query interception — after pointer translation
    // so that pointer args in queries (glGetIntegerv, etc.) point to host
    // memory. Only intercept functions tagged QUERY in the spec.
    bool is_gl_query = (entry.spec &&
                        entry.spec->policy == thunk::Policy::QUERY);
    bool handled_by_tracker = false;
    if (is_gl_query && impl_->gl_state_tracker_) {
        // Pass the post-translation `args` so the tracker writes query
        // results into the host buffer (direct-window alias or bounce)
        // that dispatch() will write back to guest memory.
        handled_by_tracker = impl_->gl_state_tracker_->try_handle_query(entry.name, args, cpu);
    }
    // ── Writeback with LIVE-RANGE CLAMP (heap-shredder fix) ───────────
    // Each bounced pointer arg is written back only up to the end of the
    // enclosing tracked guest allocation, and never at all when no live
    // allocation encloses it (untracked space must not be written).
    // A pointer only bounces when its default-size range is not one
    // accessible host range; the old unconditional full-bounce writeback
    // sprayed up to 64 KiB of stale bytes over adjacent live allocations
    // per thunk call (vkQuake/mimalloc arenas).
    auto safe_writeback_all = [&]() {
        if (!impl_->mem) return;
        static int wb_skip_diag = 4, wb_clamp_diag = 4;
        // one snapshot per dispatch, not per arg — and none at all when
        // every pointer aliased the window (the common case: no bounce
        // means no writeback, so the map copy + lock is pure overhead
        // at ~10M thunk calls/s).
        bool any_wb = false;
        for (int i = 0; i < kMaxArgs; i++) {
            if (bounce_wb[i] && bounce_guest[i]) { any_wb = true; break; }
        }
        if (!any_wb) return;
        auto live = impl_->mem->allocations_snapshot();
        for (int i = 0; i < kMaxArgs; i++) {
            if (!bounce_wb[i] || !bounce_guest[i]) continue;
            const uint64_t g = bounce_guest[i];
            const size_t want = bounce_bufs[i].size();
            size_t wb = 0;
            bool in_live = false;
            for (const auto& kv : live) {
                if (g >= kv.first && g - kv.first < kv.second) {
                    uint64_t room = kv.second - (g - kv.first);
                    wb = (want <= room) ? want : static_cast<size_t>(room);
                    in_live = true;
                    break;
                }
            }
            if (!in_live) {
                if (wb_skip_diag > 0) {
                    --wb_skip_diag;
                    fprintf(stderr,
                            "[thunk] wb-skip %s arg%d @0x%llx: target "
                            "UNTRACKED, %zu B writeback dropped\n",
                            entry.name.c_str(), i,
                            (unsigned long long)g, want);
                }
                continue;
            }
            if (wb != want && wb_clamp_diag > 0) {
                --wb_clamp_diag;
                fprintf(stderr,
                        "[thunk] wb-clamp %s arg%d @0x%llx: %zu → %zu B "
                        "(live allocation end)\n",
                        entry.name.c_str(), i, (unsigned long long)g,
                        want, wb);
            }
            try {
                impl_->mem->write(g, bounce_bufs[i].data(), wb);
            } catch (...) { /* best-effort: host call already happened */ }
        }
    };

    if (handled_by_tracker) {
        // Skip host call, but still write back any bounced pointer args
        // so the guest sees the query result.
        safe_writeback_all();
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] dispatch: %s handled by GL state tracker\n",
                    entry.name.c_str());
        }
        return 0;
    }

    // SurfaceView's OPAQUE flag is independent of its EGLConfig alpha bits.
    // Android's compositor ignores buffer alpha on that layer; a Wayland
    // compositor otherwise exposes the desktop when Unity clears alpha to 0.
    // Copy the guest list without mutating it, preserving colorspace and other
    // attributes. This is presentation policy, not a change to GL pixels.
    std::vector<int32_t> android_window_attribs;
#if defined(BIFROST_THUNK_HAVE_EGL)
    if (android_surface_handle &&
        frost::AndroidSurfaceManager::instance().surface_opaque(android_surface_handle)) {
        constexpr int32_t present_opaque = 0x31DF; // EGL_EXT_present_opaque
        auto display = reinterpret_cast<EGLDisplay>(args[0]);
        const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
        bool supported = false;
        if (extensions) {
            constexpr const char* name = "EGL_EXT_present_opaque";
            const size_t length = std::strlen(name);
            for (const char* at = extensions; (at = std::strstr(at, name)); at += length)
                if ((at == extensions || at[-1] == ' ') &&
                    (at[length] == ' ' || at[length] == '\0')) { supported = true; break; }
        }
        EGLint alpha_bits = 0;
        const bool no_alpha = eglGetConfigAttrib(display, reinterpret_cast<EGLConfig>(args[1]),
                                                 EGL_ALPHA_SIZE, &alpha_bits) && alpha_bits == 0;
        if (supported || !no_alpha) {
            if (!supported)
                fprintf(stderr, "[android] opaque RGBA presentation requires EGL_EXT_present_opaque\n");
            // On an unsupported host, EGL rejects the attribute with
            // EGL_BAD_ATTRIBUTE instead of silently creating a transparent
            // surface. Alpha-free configs need no extension.
            const uint64_t guest_list = cpu.regs[3];
            bool terminated = !guest_list;
            for (size_t i = 0; guest_list && i < 256; ++i) {
                int32_t key = impl_->mem->load<int32_t>(guest_list + i * 8);
                if (key == EGL_NONE) { terminated = true; break; }
                int32_t value = impl_->mem->load<int32_t>(guest_list + i * 8 + 4);
                if (key != present_opaque) {
                    android_window_attribs.push_back(key);
                    android_window_attribs.push_back(value);
                }
            }
            if (terminated) {
                android_window_attribs.push_back(present_opaque);
                android_window_attribs.push_back(EGL_TRUE);
            } else {
                // Bound malformed lists and let EGL report BAD_ATTRIBUTE.
                android_window_attribs = {-1, 0};
            }
            android_window_attribs.push_back(EGL_NONE);
            args[3] = reinterpret_cast<uint64_t>(android_window_attribs.data());
        }
    }
#endif

    // Bracket EGL calls to distinguish blocked calls from failed swaps.
    // Never query eglGetError here: that would consume the guest's error.
    const bool trace_present = dbg().frame_trace &&
        (entry.name == "eglSwapBuffers" || entry.name == "eglCreateWindowSurface" ||
         entry.name == "eglDestroySurface" || entry.name == "eglMakeCurrent" ||
         entry.name == "eglGetError" || entry.name == "eglTerminate");
    auto present_now_us = []() -> int64_t {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const int64_t present_started = trace_present ? present_now_us() : 0;
    if (trace_present)
        fprintf(stderr, "[present] begin mono_us=%lld tid=%u pc=0x%llx lr=0x%llx %s a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx\n",
                static_cast<long long>(present_started), static_cast<unsigned>(cpu.tid),
                static_cast<unsigned long long>(cpu.pc),
                static_cast<unsigned long long>(cpu.regs[30]), entry.name.c_str(),
                static_cast<unsigned long long>(args[0]), static_cast<unsigned long long>(args[1]),
                static_cast<unsigned long long>(args[2]), static_cast<unsigned long long>(args[3]));
    uint64_t ret = 0;
    // PCWFC: before any call that consumes buffer data (draw/copy/get/
    // texbuffer), push every live persistent+coherent bounce back to the
    // host so the GPU reads the guest's per-frame writes. Coherent mapping
    // semantics: the server sees client writes at the moment it reads them.
    // The consumer set is the spec's SYNC column (table-driven).
    if (entry.spec && entry.spec->sync_before &&
        !impl_->gl_buffer_mappings_.empty()) {
        sync_persistent_mappings_();
    }
    if (entry.name == "glTexImage2D" && dbg().thunk_trace) {
        static int full_tex_trace_n = 0;
        if (full_tex_trace_n++ < 80)
            fprintf(stderr, "[thunk] glTexImage2D call: %llx %llx %llx %llx %llx %llx %llx %llx %llx\n",
                    (unsigned long long)args[0], (unsigned long long)args[1],
                    (unsigned long long)args[2], (unsigned long long)args[3],
                    (unsigned long long)args[4], (unsigned long long)args[5],
                    (unsigned long long)args[6], (unsigned long long)args[7],
                    (unsigned long long)args[8]);
    }
    // GL string queries (GET_STRING policy) have a single GLenum argument
    // (glGetString) or GLenum+GLuint (glGetStringi) and a const GLubyte*
    // return.  Do not route them through the generic integer Fn8 call: the
    // result is a host pointer which is consumed by the string-cache path
    // below, and an untyped variadic-shaped call is especially fragile on
    // hosts where glGetString is dispatched through a GL ABI wrapper.
    // Calling it with its exact prototype also keeps the pname intact.
    bool is_get_string = entry.spec &&
                         entry.spec->policy == thunk::Policy::GET_STRING;
    if (is_get_string && entry.spec->args &&
        entry.spec->args[1] == '\0') {  // "i" — glGetString
        using GetStringFn = const unsigned char* (*)(unsigned int);
        const unsigned char* s = entry.host_fn
            ? reinterpret_cast<GetStringFn>(entry.host_fn)(
                  static_cast<unsigned int>(args[0]))
            : nullptr;
        ret = reinterpret_cast<uint64_t>(s);
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] %s pname=0x%x host='%.*s'\n",
                    entry.name.c_str(),
                    static_cast<unsigned int>(args[0]), 48,
                    s ? reinterpret_cast<const char*>(s) : "(null)");
        }
    } else if (is_get_string) {         // "ii" — glGetStringi
        using GetStringIFn = const unsigned char* (*)(unsigned int, unsigned int);
        const unsigned char* s = entry.host_fn
            ? reinterpret_cast<GetStringIFn>(entry.host_fn)(
                  static_cast<unsigned int>(args[0]),
                  static_cast<unsigned int>(args[1]))
            : nullptr;
        ret = reinterpret_cast<uint64_t>(s);
    } else if (entry.n_stack >= 7) {
        using Fn15 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t);
        ret = reinterpret_cast<Fn15>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7],
            args[8], args[9], args[10], args[11],
            args[12], args[13], args[14]);
    } else if (entry.n_stack >= 4) {
        using Fn12 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t);
        ret = reinterpret_cast<Fn12>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7],
            args[8], args[9], args[10], args[11]);
    } else if (entry.n_stack == 3) {
        using Fn11 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t);
        ret = reinterpret_cast<Fn11>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7],
            args[8], args[9], args[10]);
    } else if (entry.n_stack == 2) {
        using Fn10 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t);
        ret = reinterpret_cast<Fn10>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7],
            args[8], args[9]);
    } else if (entry.n_stack == 1) {
        using Fn9 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t);
        ret = reinterpret_cast<Fn9>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7], args[8]);
    } else {
        using Fn8 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t);
        ret = reinterpret_cast<Fn8>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7]);
    }

    if (trace_present) {
        const int64_t finished = present_now_us();
        fprintf(stderr, "[present] end mono_us=%lld tid=%u %s ret=0x%llx duration_us=%lld\n",
                static_cast<long long>(finished), static_cast<unsigned>(cpu.tid),
                entry.name.c_str(), static_cast<unsigned long long>(ret),
                static_cast<long long>(finished - present_started));
    }

    // Write bounced pointer args back into guest memory (clamped to the
    // enclosing live allocation — see safe_writeback_all above).
    safe_writeback_all();

    static int gui_dbg_n_ = -1;
    if (gui_dbg_n_ < 0) gui_dbg_n_ = getenv("BIFROST_GUI_DBG") ? 4000 : 0;
    if (gui_dbg_n_ > 0) {
        --gui_dbg_n_;
        fprintf(stderr, "[g] %s(%llx,%llx,%llx,%llx)%s\n",
                entry.name.c_str(),
                (unsigned long long)args[0], (unsigned long long)args[1],
                (unsigned long long)args[2], (unsigned long long)args[3],
                "");
    }
    if (entry.name == "SDL_GL_SwapWindow" && getenv("BIFROST_FB_DUMP")) {
        static int frame_n = 0;
        static bool warned = false;
        if (!warned) {
            warned = true;
            fprintf(stderr,
                    "[fbdump] BIFROST_FB_DUMP is set — frames WILL stall for "
                    "screenshots. Unset it for normal gameplay!\n");
        }
        // BIFROST_FB_DUMP="1" → default list; "300,600,900" → explicit.
        static const int* dump_frames = nullptr;
        static int dump_n = 0;
        if (!dump_frames) {
            static int list_buf[64];
            int n = 0;
            const char* spec = getenv("BIFROST_FB_DUMP");
            if (spec && strcmp(spec, "1") != 0) {
                for (const char* p = spec; *p && n < 64; ) {
                    char* end = nullptr;
                    long v = strtol(p, &end, 10);
                    if (end == p) break;
                    list_buf[n++] = static_cast<int>(v);
                    p = end;
                    while (*p == ',' || *p == ' ') p++;
                }
            }
            if (n == 0) {
                static const int def[] = {20, 100, 300, 600, 1000, 1500};
                memcpy(list_buf, def, sizeof(def));
                n = 6;
            }
            dump_frames = list_buf;
            dump_n = n;
        }
        bool do_dump = false;
        int dump_at = 0;
        for (int di = 0; di < dump_n; di++)
            if (frame_n == dump_frames[di]) {
                dump_at = dump_frames[di];
                do_dump = true;
                break;
            }
        if (do_dump) {
            using GSzFn = void (*)(void*, int*, int*);
            auto gsz = reinterpret_cast<GSzFn>(
                dlsym(RTLD_DEFAULT, "SDL_GL_GetDrawableSize"));
            auto rpx = reinterpret_cast<void (*)(int,int,int,int,
                          unsigned,unsigned,void*)>(
                dlsym(RTLD_DEFAULT, "glReadPixels"));
            int dw = 0, dh = 0;
            if (gsz) gsz(reinterpret_cast<void*>(args[0]), &dw, &dh);
            if (rpx && dw > 0 && dh > 0 && impl_->mem) {
                std::vector<uint8_t> px((size_t)dw * dh * 4);
                rpx(0, 0, dw, dh, 0x1908 /*GL_RGBA*/, 0x1401 /*UNSIGNED_BYTE*/,
                    px.data());
                char fbpath[64];
                snprintf(fbpath, sizeof(fbpath), "/tmp/opencode/fb%d.ppm",
                         dump_at);
                FILE* f = fopen(fbpath, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", dw, dh);
                    for (int i = 0; i < dw * dh; i++)
                        fwrite(&px[i*4], 1, 3, f);
                    fclose(f);
                    fprintf(stderr, "[fbdump] wrote %dx%d\n", dw, dh);
                }
            }
        }
        ++frame_n;
    }
    // 1.5.4-alpha: GLFW event pump — after the host poll/ wait returns,
    // deliver any registered guest cursor-position callbacks (the game
    // computes per-frame mouse deltas from them, which drives camera
    // look). The guest callback runs via the borrow-CPU runner.
    if (entry.spec && entry.spec->policy == thunk::Policy::GLFW_POLL) {
        impl_->deliver_glfw_callbacks_(cpu);
    }

    if (entry.name == "SDL_PumpEvents" || entry.name == "SDL_PollEvent" ||
        entry.name == "SDL_WaitEvent" || entry.name == "SDL_WaitEventTimeout" ||
        entry.name == "SDL_PeepEvents" || entry.name == "SDL_ResetKeyboard")
        impl_->refresh_keyboard();

    // Track SDL_Texture* dimensions for SDL_UpdateTexture bounce sizing
    // (TRACK_TEX records on create, UNTRACK_TEX drops on destroy).
    if (entry.spec) {
        if (entry.spec->policy == thunk::Policy::TRACK_TEX) {
            std::lock_guard<std::mutex> lk(impl_->state_mu);
            impl_->sdl_tex_sizes_[ret] = {static_cast<uint32_t>(args[3]),
                                          static_cast<uint32_t>(args[4])};
        } else if (entry.spec->policy == thunk::Policy::UNTRACK_TEX) {
            std::lock_guard<std::mutex> lk(impl_->state_mu);
            impl_->sdl_tex_sizes_.erase(args[0]);
        }
    }

    // Track host SDL semaphores the guest creates so wake_sdl_semaphores()
    // can release a worker thread blocked in SDL_SemWait when the guest
    // exits (exit_group only stops the calling CPU). SDL_CreateSemaphore
    // returns the host SDL_sem*; SDL_DestroySemaphore takes it as a0.
    if (entry.name == "SDL_CreateSemaphore") {
        if (ret) {
            std::lock_guard<std::mutex> g(impl_->mu);
            impl_->sdl_sems_.insert(ret);
        }
    } else if (entry.name == "SDL_DestroySemaphore") {
        std::lock_guard<std::mutex> g(impl_->mu);
        impl_->sdl_sems_.erase(args[0]);
    }

    // glDeleteBuffers(n, names) [DELETE_BUFFERS policy]: free any live
    // mappings (persistent bounces are kept alive across unmap, so this
    // is their only release point).
    if (entry.spec && entry.spec->policy == thunk::Policy::DELETE_BUFFERS &&
        impl_->mem) {
        uint32_t n = static_cast<uint32_t>(args[0]);
        if (n > 0 && n < 1024 && args[1]) {
            const uint32_t* names =
                reinterpret_cast<const uint32_t*>(args[1]);
            // Erase under the lock (so concurrent map/unmap can't see a
            // half-freed mapping), then untrack the bounces OUTSIDE it —
            // same pattern as the UNMAP_BUFFER arm (untrack_allocation
            // touches Memory's own locks and must not run under impl_->mu).
            std::vector<GraphicThunkImpl::BufferMapping> freed;
            {
                std::lock_guard<std::mutex> g(impl_->mu);
                for (uint32_t i = 0; i < n; i++) {
                    auto it = impl_->gl_buffer_mappings_.find(names[i]);
                    if (it != impl_->gl_buffer_mappings_.end()) {
                        freed.push_back(it->second);
                        impl_->gl_buffer_mappings_.erase(it);
                    }
                }
            }
            for (auto& m : freed)
                if (m.bounce) impl_->mem->untrack_allocation(m.bounce, m.size);
            // Mirror into the state tracker: deleting a bound buffer
            // unbinds it (else a later re-generated name would observe a
            // stale "already bound" and get elided). args[1] is host-
            // readable here (bounce or window alias).
            if (impl_->gl_state_tracker_)
                impl_->gl_state_tracker_->unbind_buffers(names, n);
        }
    }

#if defined(BIFROST_THUNK_HAVE_SDL2)
    if (dbg().input_trace && (entry.name == "SDL_SetRelativeMouseMode" ||
                             entry.name == "SDL_SetWindowGrab"))
        fprintf(stderr, "[input] %s arg=%llu result=%d\n", entry.name.c_str(),
                static_cast<unsigned long long>(args[entry.name == "SDL_SetWindowGrab" ? 1 : 0]),
                static_cast<int32_t>(ret));
    if (entry.name == "SDL_PollEvent" && dbg().input_trace && !dbg().input_watch.empty()) {
        static thread_local std::vector<uint32_t> last;
        static thread_local unsigned captures = 0;
        if (captures < 4096) {
            std::vector<uint32_t> values;
            bool readable = true;
            for (uint64_t address : dbg().input_watch) {
                uint32_t value = 0;
                try { impl_->mem->read(address, &value, sizeof(value)); }
                catch (...) { readable = false; break; }
                values.push_back(value);
            }
            if (readable && values != last) {
                fprintf(stderr, "[input] guest-state");
                for (size_t i = 0; i < values.size(); ++i)
                    fprintf(stderr, " %llx=%08x", static_cast<unsigned long long>(dbg().input_watch[i]), values[i]);
                fprintf(stderr, "\n");
                last = std::move(values);
                ++captures;
            }
        }
    }
    if (entry.name == "SDL_PollEvent" && (dbg().thunk_trace || dbg().input_trace) &&
        ret && args[0]) {
        SDL_Event event;
        const void* hp = bounce_guest[0] ? bounce_bufs[0].data()
                                        : reinterpret_cast<void*>(args[0]);
        std::memcpy(&event, hp, sizeof(event));
        if (event.type == SDL_MOUSEMOTION) {
            static std::atomic<unsigned> motion_count{0};
            // Bound opt-in capture so a long play session cannot grow an
            // input log without limit. Full thunk tracing remains unlimited.
            unsigned count = dbg().input_trace ? motion_count.fetch_add(1) : 0;
            if (dbg().thunk_trace || count < 4096)
                fprintf(stderr, "[input] motion window=%u device=%u state=%u x=%d y=%d dx=%d dy=%d relative=%d\n",
                        event.motion.windowID, event.motion.which, event.motion.state,
                        event.motion.x, event.motion.y, event.motion.xrel, event.motion.yrel,
                        SDL_GetRelativeMouseMode());
        } else if (dbg().input_trace && event.type == SDL_WINDOWEVENT) {
            fprintf(stderr, "[input] window=%u event=%u data=%d,%d\n",
                    event.window.windowID, event.window.event, event.window.data1, event.window.data2);
        }
    }
#endif
    if (entry.spec && entry.spec->ret == thunk::RetKind::STRING) {
        ret = impl_->cache_host_string_(reinterpret_cast<const char*>(ret));
    }
    if (android_surface_handle && ret)
        frost::AndroidSurfaceManager::instance().register_egl_surface(ret, android_surface_handle);
    if (entry.name == "eglDestroySurface" && ret)
        frost::AndroidSurfaceManager::instance().destroy_egl_surface(args[1]);
    window_stats::api(entry.name,args,ret);
    if (entry.name == "eglSwapBuffers" && ret &&
        !frost::AndroidSurfaceManager::instance().present_egl_surface(args[1])) {
#if defined(BIFROST_THUNK_HAVE_SDL2)
        window_stats::sdl_present(impl_->sdl_window,"EGL");
#endif
    }
    cpu.regs[0] = ret;
    if (impl_->gl_state_tracker_ && entry.tracks_state) {
        impl_->gl_state_tracker_->track_state_change(entry.name, args, nullptr, 0);
    }
    // Lightweight frame counter: print every 5 present/swap calls when
    // BIFROST_FRAME_TRACE=1 (avoids the heavy per-call dispatch trace).
    // Gate is cached in dbg() — getenv() on every dispatch was measurable.
    if (dbg().frame_trace && entry.spec &&
        entry.spec->policy == thunk::Policy::PRESENT) {
        static uint64_t frame_count = 0;
        static uint64_t t0 = 0;
        auto now_us = []() -> uint64_t {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
        if (frame_count == 0) t0 = now_us();
        if (++frame_count % 5 == 0) {
            uint64_t dt = now_us() - t0;
            fprintf(stderr, "[frame] %s: %llu frames in %.2fs (%.2f fps)\n",
                    entry.name.c_str(),
                    static_cast<unsigned long long>(frame_count),
                    dt / 1e6,
                    frame_count * 1e6 / static_cast<double>(dt));
        }
    }
    return 0;
}
}  // namespace arm64emu
