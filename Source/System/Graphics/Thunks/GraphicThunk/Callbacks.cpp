// dispatch() stage 2: GLFW and GL debug callbacks. Guest AArch64 callbacks are
// stored and delivered from the poll path, never handed to the host setter.
#include "System/Graphics/Thunks/GraphicThunk/GraphicThunkInternal.hpp"
namespace arm64emu {
std::optional<int64_t> GraphicThunk::dispatch_guest_callbacks_(CPU& cpu, const SymbolEntry& entry) {
    // ── GLFW callback registration (CURSOR_CB/KEY_CB/MOUSE_CB/…) ─────
    // 1.5.4-alpha: the guest callback is AArch64 code host GLFW cannot
    // invoke, so we store it keyed by window and deliver it from the
    // GLFW_POLL path. NEVER hand the guest address to host
    // glfwSetCursorPosCallback etc — the host would call it as x86-64
    // (SIGSEGV). cpu.regs[0]=window handle (except ERROR_CB, global),
    // cpu.regs[1]=guest callback.
    if (entry.spec) {
        thunk::Policy pol = entry.spec->policy;
        if (pol == thunk::Policy::CURSOR_CB || pol == thunk::Policy::KEY_CB ||
            pol == thunk::Policy::MOUSE_CB || pol == thunk::Policy::FRAMEBUFFER_CB ||
            pol == thunk::Policy::WINDOW_SIZE_CB || pol == thunk::Policy::FOCUS_CB ||
            pol == thunk::Policy::ERROR_CB) {
            if (pol == thunk::Policy::ERROR_CB) {
                // Global callback, single arg: glfwSetErrorCallback(cb).
                {
                    std::lock_guard<std::mutex> lk(impl_->state_mu);
                    impl_->glfw_error_cb_ = cpu.regs[0];
                }
                if (dbg().thunk_trace) {
                    fprintf(stderr, "[thunk] glfwSetErrorCallback cb=0x%llx\n",
                            static_cast<unsigned long long>(cpu.regs[0]));
                }
                cpu.regs[0] = 0;
                return 0;
            }
            uint64_t window = cpu.regs[0];
            uint64_t guest_cb = cpu.regs[1];
            {
                std::lock_guard<std::mutex> lk(impl_->state_mu);
                auto& cbs = impl_->glfw_cbs_[window];
                uint64_t* slot = nullptr;
                switch (pol) {
                    case thunk::Policy::CURSOR_CB:       slot = &cbs.cursor; break;
                    case thunk::Policy::KEY_CB:          slot = &cbs.key; break;
                    case thunk::Policy::MOUSE_CB:        slot = &cbs.mouse; break;
                    case thunk::Policy::FRAMEBUFFER_CB:  slot = &cbs.framebuffer; break;
                    case thunk::Policy::WINDOW_SIZE_CB:  slot = &cbs.window_size; break;
                    case thunk::Policy::FOCUS_CB:        slot = &cbs.focus; break;
                    default: break;
                }
                if (slot) {
                    *slot = guest_cb;
                    if (guest_cb == 0) {
                        // Unregister: drop the window entry if all slots empty.
                        if (cbs.cursor == 0 && cbs.key == 0 && cbs.mouse == 0 &&
                            cbs.framebuffer == 0 && cbs.window_size == 0 &&
                            cbs.focus == 0) {
                            impl_->glfw_cbs_.erase(window);
                        }
                    }
                }
            }
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] %s: window=0x%llx cb=0x%llx\n",
                        entry.name.c_str(),
                        static_cast<unsigned long long>(window),
                        static_cast<unsigned long long>(guest_cb));
            }
            cpu.regs[0] = 0;
            return 0;
        }
        // ── GL_KHR_debug callback (GL_DEBUG_CB) ────────────────────
        // glDebugMessageCallback(cb, userParam): the callback is AArch64
        // code the host GL driver cannot invoke. Store it (delivery is a
        // future milestone) and do NOT call the host setter — handing a
        // guest address to host GL crashes on the first driver message.
        // The host context keeps debug output disabled (the default).
        if (pol == thunk::Policy::GL_DEBUG_CB) {
            {
                std::lock_guard<std::mutex> lk(impl_->state_mu);
                impl_->gl_debug_cb_ = cpu.regs[0];
                impl_->gl_debug_param_ = cpu.regs[1];
            }
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] glDebugMessageCallback cb=0x%llx param=0x%llx (stored, host untouched)\n",
                        static_cast<unsigned long long>(cpu.regs[0]),
                        static_cast<unsigned long long>(cpu.regs[1]));
            }
            cpu.regs[0] = 0;
            return 0;
        }
    }
    return std::nullopt;
}
}  // namespace arm64emu
