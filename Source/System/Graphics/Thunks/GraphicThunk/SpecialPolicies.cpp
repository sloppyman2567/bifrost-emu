// dispatch() stage 1: keyboard state, SDL_mixer/SDL stubs, thread and semaphore
// policies, and symbols with no host function.
#include "System/Graphics/Thunks/GraphicThunk/GraphicThunkInternal.hpp"
namespace arm64emu {
std::optional<int64_t> GraphicThunk::dispatch_special_policies_(CPU& cpu, const SymbolEntry& entry) {
    if (entry.name == "SDL_GetKeyboardState") {
        uint64_t out = cpu.regs[0];
        uint64_t guest = impl_->refresh_keyboard();
        int count;
        { std::lock_guard<std::mutex> lock(impl_->keyboard_mu); count = impl_->keyboard_count; }
        if (out) impl_->mem->write(out, &count, sizeof(count));
        cpu.regs[0] = guest;
        return 0;
    }

    // ── SDL_mixer policies ────────────────────────────────────────────
    // SDL_mixer is never linked into the emulator, so every Mix_* entry's
    // host_fn is NULL and the generic stub path below would return 0. Two
    // symbols need non-zero semantics:
    //   MIX_VERSION   -> Mix_Linked_Version(): the game dereferences the
    //                    returned SDL_version* (reads major/minor/patch),
    //                    so return a guest-addressable SDL_version{2,0,1}.
    //   MIX_OPEN_AUDIO-> Mix_OpenAudio/OpenAudioDevice: return -1 so the
    //                    game takes its graceful no-audio path (it checks
    //                    for != 0 and disables sound/music) instead of
    //                    believing audio is open.
    if (entry.spec) {
        thunk::Policy pol = entry.spec->policy;
        if (pol == thunk::Policy::MIX_VERSION) {
            // SDL_version is {Uint8 major, Uint8 minor, Uint8 patch}.
            static const uint8_t kVer[8] = {2, 0, 1, 0, 0, 0, 0, 0};
            uint64_t guest = impl_->cache_host_bytes_(kVer, sizeof(kVer));
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] Mix_Linked_Version -> {2,0,1} @ 0x%llx\n",
                        static_cast<unsigned long long>(guest));
            }
            cpu.regs[0] = guest;
            return 0;
        }
        if (pol == thunk::Policy::MIX_OPEN_AUDIO) {
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] %s -> -1 (audio unavailable)\n",
                        entry.name.c_str());
            }
            cpu.regs[0] = static_cast<uint64_t>(-1);
            return 0;
        }
        // NOTE: SDL2 audio symbols (SDL_OpenAudio/QueueAudio/etc.) are
        // intentionally NOT registered here — they live in the AudioThunk
        // registry (callback mode + pump thread), and the dynlinker
        // resolves them to AudioThunk trampolines directly.
        if (!strcmp(entry.name.c_str(), "SDL_Vulkan_GetVkGetInstanceProcAddr")) {
            // vkQuake stores this result and calls it to load EVERY
            // Vulkan entry point — it must be a GUEST-CALLABLE trampoline
            // (DisplayThunk registry), never the raw host fn pointer.
            cpu.regs[0] = impl_->vk_proc_lookup_
                ? impl_->vk_proc_lookup_("vkGetInstanceProcAddr") : 0;
            return 0;
        }
        if (pol == thunk::Policy::SDL_STUB0) {
            // SDL_GetWindowWMInfo is FATAL-failed by vkQuake when it
            // returns false, so it must reach the host: the window handle
            // round-trips verbatim and the SDL_SysWMinfo buffer lives on
            // the guest stack (inside the direct window) -> alias pass.
            // The host pointers the host writes into it are opaque to the
            // game (it only checks the BOOL return).
            const char* nm = entry.name.c_str();
            if (!strcmp(nm, "SDL_qsort") && dbg().thunk_trace) {
                // Safe stub: the generic path would hand the AArch64
                // comparator to host qsort (SIGSEGV on first compare), so
                // this row is SDL_STUB0 and sorts nothing. No Tests/Unit/src
                // caller depends on it; a borrow-CPU comparator arm would
                // be disproportionate until a real game needs it.
                fprintf(stderr, "[thunk] SDL_qsort: no-op, array left "
                        "unsorted (guest comparator cannot run on host)\n");
            }
            if (!strcmp(nm, "SDL_GetWindowWMInfo")) {
                using WmFn = int (*)(void*, void*);
                auto fn = reinterpret_cast<WmFn>(entry.host_fn);
                uint8_t* hp = (impl_ && impl_->mem)
                    ? impl_->mem->guest_to_host_ptr(cpu.regs[1]) : nullptr;
                cpu.regs[0] = (fn && hp)
                    ? static_cast<uint64_t>(fn(reinterpret_cast<void*>(
                          cpu.regs[0]), hp))
                    : 0;
                return 0;
            }
            cpu.regs[0] = 0;
            return 0;
        }
        if (pol == thunk::Policy::THREAD_ID) {
            // SDL_Thread handles belong to our guest runner, never host SDL.
            cpu.regs[0] = (entry.name == "SDL_ThreadID" || cpu.regs[0] == 0)
                ? static_cast<uint64_t>(cpu.tid)
                : (impl_->sdl_thread_runner_
                    ? impl_->sdl_thread_runner_(cpu, 3, cpu.regs[0], 0, 0) : 0);
            return 0;
        }
        if (pol == thunk::Policy::SDL_EVENT_FILTER) {
#if defined(BIFROST_THUNK_HAVE_SDL2)
            if (entry.name == "SDL_SetEventFilter") {
                {
                    std::lock_guard<std::mutex> lock(impl_->state_mu);
                    impl_->sdl_filter_fn_ = cpu.regs[0];
                    impl_->sdl_filter_userdata_ = cpu.regs[1];
                }
                // Let SDL apply its queue reset and filter-installation semantics.
                SDL_SetEventFilter(cpu.regs[0] ? GraphicThunkImpl::sdl_event_filter_ : nullptr,
                                   cpu.regs[0] ? impl_.get() : nullptr);
                cpu.regs[0] = 0; // void API
            } else {
                uint64_t fn, data;
                {
                    std::lock_guard<std::mutex> lock(impl_->state_mu);
                    fn = impl_->sdl_filter_fn_; data = impl_->sdl_filter_userdata_;
                }
                if (cpu.regs[0]) impl_->mem->write(cpu.regs[0], &fn, sizeof(fn));
                if (cpu.regs[1]) impl_->mem->write(cpu.regs[1], &data, sizeof(data));
                cpu.regs[0] = fn != 0;
            }
#else
            cpu.regs[0] = 0;
#endif
            return 0;
        }
        if (pol == thunk::Policy::NONE && impl_ && impl_->mem) {
            // SDL2 display-mode queries are int(index, SDL_DisplayMode *out).
            // The generic pointer path writes the 24-byte LP64 struct into
            // the caller's buffer and returns the host status unchanged.
            // Never substitute the SDL3 pointer-returning API here.
            const char* nm = entry.name.c_str();
            // SDL_RWFromMem / SDL_RWFromConstMem(mem, size): SDL KEEPS the
            // mem pointer in the RWops and reads it on every later
            // SDL_LoadBMP_RW/etc. A temporary per-call bounce buffer dies
            // at dispatch return → use-after-free + heap corruption in
            // whatever reallocates that memory. Use the stable direct-
            // window alias when possible; otherwise take a persistent
            // host copy (tracked, freed at thunk shutdown).
            if (strcmp(nm, "SDL_RWFromMem") == 0 ||
                strcmp(nm, "SDL_RWFromConstMem") == 0) {
                using RwFn = void* (*)(void*, size_t);
                auto fn = reinterpret_cast<RwFn>(entry.host_fn);
                if (!fn) { cpu.regs[0] = 0; return 0; }
                uint64_t src = cpu.regs[0];
                size_t sz = static_cast<size_t>(cpu.regs[1]);
                uint8_t* hp = impl_->mem->guest_to_host_ptr(src);
                if (!hp && sz && sz < (64u << 20)) {
                    hp = static_cast<uint8_t*>(std::malloc(sz));
                    if (hp) {
                        try { impl_->mem->read(src, hp, sz); }
                        catch (...) { std::memset(hp, 0, sz); }
                        std::lock_guard<std::mutex> lk(impl_->mu);
                        impl_->rw_kept_.emplace_back(hp);
                    }
                }
                if (!hp) { cpu.regs[0] = 0; return 0; }
                cpu.regs[0] = reinterpret_cast<uint64_t>(fn(hp, sz));
                return 0;
            }
        }
        if (pol == thunk::Policy::SDLVK_EXT) {
            // SDL_Vulkan_GetInstanceExtensions(window, pCount*, pNames*):
            // pNames is the CALLER'S ARRAY to fill directly with char*
            // (NOT a slot receiving a new char**). Fill it with guest
            // string-cache copies of the host names.
            using VkExtFn = int (*)(void*, unsigned*, const char**);
            auto fn = reinterpret_cast<VkExtFn>(entry.host_fn);
            if (!fn || !impl_->mem) { cpu.regs[0] = 0; return 0; }
            void* win = reinterpret_cast<void*>(cpu.regs[0]);
            uint64_t pcount = cpu.regs[1], pnames = cpu.regs[2];
            unsigned count = 0;
            bool ok1 = fn(win, &count, nullptr) != 0;
            if (dbg().thunk_trace)
                fprintf(stderr, "[thunk] SDLVK_EXT: win=%p ok=%d count=%u pnames=0x%llx\n",
                        win, ok1 ? 1 : 0, count, (unsigned long long)pnames);
            if (!ok1) { cpu.regs[0] = 0; return 0; }
            if (count > 64) { cpu.regs[0] = 0; return 0; }
            if (pnames && count) {
                std::vector<const char*> names(count, nullptr);
                if (!fn(win, &count, names.data())) {
                    cpu.regs[0] = 0;
                    return 0;
                }
                for (unsigned i = 0; i < count; i++) {
                    uint64_t gs = impl_->cache_host_string_(names[i]);
                    if (dbg().thunk_trace)
                        fprintf(stderr, "[thunk] SDLVK_EXT: names[%u]=%s -> gs=0x%llx\n",
                                i, names[i] ? names[i] : "(null)", (unsigned long long)gs);
                    impl_->mem->write(pnames + static_cast<uint64_t>(i) * 8,
                                      &gs, 8);
                }
            }
            if (pcount) impl_->mem->write(pcount, &count, sizeof(count));
            cpu.regs[0] = 1;
            return 0;
        }
        if (pol == thunk::Policy::SDL_ALLOC) {
            // SDL_malloc/calloc/realloc: return GUEST-window memory so the
            // guest can deref the pointer. mmap_alloc is zero-filled
            // (calloc semantics for free). realloc copies the old block's
            // contents (size looked up from the allocation map).
            uint64_t ret = 0;
            if (impl_ && impl_->mem) {
                const char* nm = entry.name.c_str();
                if (!strcmp(nm, "SDL_malloc")) {
                    ret = impl_->mem->mmap_alloc(cpu.regs[0]);
                } else if (!strcmp(nm, "SDL_calloc")) {
                    uint64_t sz = 0;
                    if (!__builtin_mul_overflow(cpu.regs[0], cpu.regs[1], &sz))
                        ret = impl_->mem->mmap_alloc(sz);
                } else { // SDL_realloc(ptr, size)
                    uint64_t oldp = cpu.regs[0], newsz = cpu.regs[1];
                    ret = impl_->mem->mmap_alloc(newsz);
                    if (oldp && ret) {
                        // Point lookup — the old code copied the whole
                        // allocation map per call (O(live allocs)).
                        auto [found, oldsz] = impl_->mem->find_allocation(oldp);
                        if (found) {
                            uint64_t n = std::min(oldsz, newsz);
                            uint8_t* hp = impl_->mem->guest_to_host_ptr(oldp);
                            uint8_t* hq = impl_->mem->guest_to_host_ptr(ret);
                            if (hp && hq) memcpy(hq, hp, n);
                            impl_->mem->untrack_allocation(oldp, oldsz);
                        }
                    }
                }
            }
            cpu.regs[0] = ret;
            return 0;
        }
        if (pol == thunk::Policy::SDL_FREE) {
            // SDL_free(ptr): the guest may free a string returned by
            // SDL_GetClipboardText / SDL_GetError / joystick-name getters,
            // which lives in the GUEST string cache — reclaim the cache slot
            // instead of calling host free() (the pointer is a guest address,
            // not a host heap allocation). Pointers handed out by SDL_ALLOC
            // (mmap_alloc'd guest-window blocks) are untracked here so the
            // page cap reflects live memory. Any other guest pointer is a
            // leak-safe no-op. Must run BEFORE the generic host-fn stub
            // check (SDL_free has no host call at all).
            if (impl_ && impl_->mem) {
                uint64_t p = cpu.regs[0];
                bool freed = false;
                if (p) {
                    auto [found, sz] = impl_->mem->find_allocation(p);
                    if (found) {
                        impl_->mem->untrack_allocation(p, sz);
                        freed = true;
                    }
                }
                if (!freed && impl_->string_cache_base)
                    impl_->free_cache_string_(p);
            } else if (impl_ && impl_->string_cache_base) {
                impl_->free_cache_string_(cpu.regs[0]);
            }
            cpu.regs[0] = 0;
            return 0;
        }
        if (pol == thunk::Policy::THREAD_CREATE) {
            // SDL_CreateThread(fn, name, data): spawn a REAL guest thread
            // running fn(data) on its own CPU. The Emulator wires the
            // runner; it returns a guest SDL_Thread* handle (or 0 on
            // failure, which the game cbz-checks → fatal error path).
            if (impl_->sdl_thread_runner_) {
                uint64_t handle = impl_->sdl_thread_runner_(
                    cpu, 0, cpu.regs[0], cpu.regs[1], cpu.regs[2]);
                if (dbg().thunk_trace) {
                    fprintf(stderr, "[thunk] SDL_CreateThread(fn=0x%llx, "
                            "name=0x%llx, data=0x%llx) -> 0x%llx\n",
                            static_cast<unsigned long long>(cpu.regs[0]),
                            static_cast<unsigned long long>(cpu.regs[1]),
                            static_cast<unsigned long long>(cpu.regs[2]),
                            static_cast<unsigned long long>(handle));
                }
                cpu.regs[0] = handle;
            } else {
                cpu.regs[0] = 0;
            }
            return 0;
        }
        if (pol == thunk::Policy::THREAD_WAIT) {
            // SDL_WaitThread(thread, status): block until the thread's
            // function returns, write its exit code to *status (if
            // non-null), and free the thread's resources.
            if (impl_->sdl_thread_runner_) {
                impl_->sdl_thread_runner_(cpu, 1, cpu.regs[0], cpu.regs[1], 0);
            }
            cpu.regs[0] = 0;
            return 0;
        }
        if (pol == thunk::Policy::THREAD_DETACH) {
            // SDL_DetachThread(thread): the guest promises never to wait on
            // this thread. The handle is a GUEST-emulated thread handle, not
            // a host SDL_Thread*, so forwarding it to host SDL would be type
            // confusion on guest memory. Tell the emulator instead so it can
            // detach its own host thread (a later ~std::thread on a joinable
            // thread calls std::terminate).
            if (impl_->sdl_thread_runner_) {
                impl_->sdl_thread_runner_(cpu, 2, cpu.regs[0], 0, 0);
            }
            cpu.regs[0] = 0;
            return 0;
        }
    }

    if (!entry.host_fn) {
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] dispatch: %s (stub, returns 0)\n",
                    entry.name.c_str());
        }
        cpu.regs[0] = 0;
        if (entry.spec && (entry.spec->ret == thunk::RetKind::FLOAT ||
                           entry.spec->ret == thunk::RetKind::DOUBLE))
            cpu.v_lo[0] = cpu.v_hi[0] = 0;
        return 0;
    }
    return std::nullopt;
}
}  // namespace arm64emu
