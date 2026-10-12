// Host-thunk wiring: GLFW/SDL/audio callback runners, the thunk linker and
// borrow-CPU calls into guest functions.
#include "System/Core/Emulator.hpp"
#include "System/Core/DebugFlags.hpp"
#include "Bifrost/Version.hpp"
#include "System/Core/CrashReport.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Loader/DynamicLinker.hpp"
#include "System/Graphics/Thunks/GraphicThunk.hpp"        // GraphicThunk full definition (for init/resolve)
#include "System/Android/Surface/Surface.hpp"// 2026-08: Android activity
#include "System/Audio/AudioThunk.hpp"  // AudioThunk
#include "System/Graphics/Thunks/DisplayThunk.hpp"// DisplayThunk
#include "System/VFS/Nodes/HostNode.hpp"  // wl fd publisher wraps duped host fds
#include <fcntl.h>
#include <unistd.h>
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
namespace arm64emu {
// Host-to-guest signal forwarding (install_host_signal_handlers,
// queue_host_signal, host_signal_handler, drain_host_signals) is
// implemented in Source/System/Core/Signals.cpp — see that file for the full
// disposition table.
// ── wire_thunk_glfw_cb_runner_ — guest GLFW callback delivery ─────────
// glfwSetCursorPosCallback/glfwSetKeyCallback/… register
// guest AArch64 callbacks; host GLFW can't invoke them, so GraphicThunk
// stores them and asks us to run them after each
// glfwPollEvents/glfwWaitEvents. We borrow the CPU the same way the
// dynamic linker's guest_call_args_ does (save/restore ALL state, run
// step() in a loop to a sentinel LR), passing the callback args in
// integer (x0..) and FP (d0..) registers:
//   void cb(GLFWwindow* w, ...)     → x0 = iargs[0] = window, …
//   void errcb(int code, char* d)   → x0 = iargs[0], …
void Emulator::wire_thunk_glfw_cb_runner_() {
    auto* thunk = graphics_.thunk();
    if (!thunk || !thunk->enabled()) return;
    thunk->set_sdl_filter_cpu_factory([this]() -> std::unique_ptr<CPU> {
        auto cpu = std::make_unique<CPU>();
        if (dyn_linker_) {
            uint64_t tls = dyn_linker_->allocate_thread_tls(mem_);
            if (!tls) return {};
            cpu->tpidr_el0 = tls;
            cpu->tpidrro_el0 = tls;
        }
        cpu->tid = next_tid_.fetch_add(1);
        if (jit_) jit_->enter_multithreaded();
        if (libc_single_threaded_addr_)
            mem_.store<uint8_t>(libc_single_threaded_addr_, 0);
        return cpu;
    });
    thunk->set_glfw_cb_runner(
        [this](CPU& cpu, uint64_t fn, const int64_t* iargs,
               size_t n_iargs, const double* fargs,
               size_t n_fargs) -> uint64_t {
            if (fn == 0) return 0;
            struct SavedState {
                uint64_t regs[32];
                uint64_t sp, pc;
                uint32_t pstate;
                uint64_t v_lo[32], v_hi[32];
                uint32_t fpcr, fpsr;
                uint64_t tpidr_el0, tpidrro_el0;
                uint64_t sigmask;
                bool running;
            } saved;
            std::memcpy(saved.regs, cpu.regs, sizeof(saved.regs));
            saved.sp = cpu.sp;
            saved.pc = cpu.pc;
            saved.pstate = cpu.pstate;
            std::memcpy(saved.v_lo, cpu.v_lo, sizeof(saved.v_lo));
            std::memcpy(saved.v_hi, cpu.v_hi, sizeof(saved.v_hi));
            saved.fpcr = cpu.fpcr;
            saved.fpsr = cpu.fpsr;
            saved.tpidr_el0 = cpu.tpidr_el0;
            saved.tpidrro_el0 = cpu.tpidrro_el0;
            saved.sigmask = cpu.sigmask;
            saved.running = cpu.running;
            auto restore = [&]() {
                std::memcpy(cpu.regs, saved.regs, sizeof(saved.regs));
                cpu.sp = saved.sp;
                cpu.pc = saved.pc;
                cpu.pstate = saved.pstate;
                std::memcpy(cpu.v_lo, saved.v_lo, sizeof(saved.v_lo));
                std::memcpy(cpu.v_hi, saved.v_hi, sizeof(saved.v_hi));
                cpu.fpcr = saved.fpcr;
                cpu.fpsr = saved.fpsr;
                cpu.tpidr_el0 = saved.tpidr_el0;
                cpu.tpidrro_el0 = saved.tpidrro_el0;
                cpu.sigmask = saved.sigmask;
                cpu.running = saved.running;
            };
            // TLS pointer: the borrowed CPU is a running guest thread,
            // so TPIDR_EL0 is already set; only initialize when missing
            // (startup edge, mirrors guest_call_args_).
            if (dyn_linker_ && dyn_linker_->static_tls_size() > 0 &&
                cpu.tpidr_el0 == 0) {
                cpu.tpidr_el0 = dyn_linker_->thread_pointer();
                cpu.tpidrro_el0 = cpu.tpidr_el0;
            }
            // Reusable scratch stack (thread-local: one per guest thread,
            // so concurrent polls from multiple vCPUs can't clobber).
            //
            // 64 KiB + 4 KiB head margin (2026-08-25): the old 8 KiB
            // scratch was allocated MID-HEAP next to live malloc chunks.
            // Guest callbacks invoked from host threads (the audio-pump
            // vCPU, GLFW/audio thunks) routinely need more than 8 KiB of
            // guest stack (SDL audio mixers nest deeply); the overflow
            // descended into the NEIGHBORING CHUNKS and shredded glibc's
            // heap ("malloc(): invalid size" / "free(): invalid size" /
            // AllocBlock shredding in vkQuake). The corruption was
            // invisible to every verification mode because it comes from
            // a host-side thread, not from any verified guest block.
            // Layout: [PROT_NONE guard][128 KiB usable][guard] — overflow
            // faults loudly instead of trashing live chunks.
            static thread_local std::vector<uint64_t> scratch_stacks;
            static thread_local size_t callback_depth = 0;
            struct CallbackDepth {
                size_t& depth;
                explicit CallbackDepth(size_t& d) : depth(d) { ++depth; }
                ~CallbackDepth() { --depth; }
            };
            size_t slot = callback_depth;
            CallbackDepth depth_scope(callback_depth);
            static thread_local const Emulator* scratch_owner = nullptr;
            if (scratch_owner != this) { scratch_stacks.clear(); scratch_owner = this; }
            if (scratch_stacks.size() <= slot) scratch_stacks.resize(slot + 1);
            uint64_t scratch_stack = scratch_stacks[slot];
            if (scratch_stack == 0) {
                scratch_stack = mem_.mmap_alloc_callback_stack(128 * 1024);
                if (scratch_stack == 0) return 0;
                scratch_owner = this;
                scratch_stacks[slot] = scratch_stack;
                if (getenv("BIFROST_SCRATCH_TRACE"))
                    fprintf(stderr, "[scratch] cpu=%p usable=[0x%llx..0x%llx)\n",
                            (void*)&cpu,
                            (unsigned long long)scratch_stack,
                            (unsigned long long)(scratch_stack + 128 * 1024));
            }
            uint64_t stack_top = scratch_stack + 128 * 1024;
            constexpr uint64_t SENTINEL_LR = 0x1000;
            cpu.pc = fn;
            cpu.sp = stack_top;
            for (size_t i = 0; i < n_iargs && i < 8; i++)
                cpu.regs[i] = static_cast<uint64_t>(iargs[i]);
            for (size_t i = 0; i < n_fargs && i < 8; i++) {
                std::memcpy(&cpu.v_lo[i], &fargs[i], sizeof(fargs[i]));
                cpu.v_hi[i] = 0;
            }
            cpu.regs[30] = SENTINEL_LR;
            cpu.running = true;
            cpu.pstate = 0;
            constexpr uint64_t CALL_LIMIT = 50'000'000;
            uint64_t steps = 0;
            uint64_t result = 0;
            try {
                while (cpu.running && cpu.pc != SENTINEL_LR &&
                       steps < CALL_LIMIT) {
                    step(cpu);
                    steps++;
                }
                if (cpu.pc == SENTINEL_LR) result = cpu.regs[0];
            } catch (const std::exception&) {
                // Callback threw (e.g. unhandled SIMD op → DecodeError).
                // Swallow: restore and let the game continue polling.
            }
            restore();
            return result;
        });
}
// ── wire_thunk_android_runner_ — Android looper/input + lifecycle ────
void Emulator::wire_thunk_android_runner_() {
    auto* dthunk = graphics_.display_thunk();
    if (!dthunk || !dthunk->enabled()) return;
    auto& mgr = frost::AndroidSurfaceManager::instance();
    mgr.set_memory(&mem_);
    mgr.set_fd_resolver([this](int gfd) -> int {
        auto n = fds_.get(gfd);
        if (!n) return -1;
        return n->host_fd();
    });
    mgr.set_runner([this](CPU& cpu, uint64_t fn, const int64_t* iargs,
                          size_t n) -> uint64_t {
        return call_guest_function(cpu, fn, iargs, n, nullptr, 0);
    });
}
// ── wire_thunk_audio_runner_ — AudioEngine + guest-callback runner ─────
void Emulator::wire_thunk_audio_runner_() {
    auto* athunk = graphics_.audio_thunk();
    if (!athunk || !athunk->enabled()) return;
    athunk->wire(&audio_,
                 [this](CPU& cpu, uint64_t fn, const int64_t* iargs,
                        size_t n) -> uint64_t {
                     return call_guest_function(cpu, fn, iargs, n,
                                                nullptr, 0);
                 },
                 [this]() -> std::unique_ptr<CPU> {
                     if (!dyn_linker_) return {};
                     auto cpu = std::make_unique<CPU>();
                     cpu->copy_arch_state_from(main_cpu_);
                     const uint64_t tls = dyn_linker_->allocate_thread_tls(mem_);
                     if (!tls) return {};
                     cpu->tpidr_el0 = tls;
                     cpu->tpidrro_el0 = tls;
                     cpu->tid = next_tid_.fetch_add(1);
                     cpu->is_fork_process = false;
                     if (jit_) jit_->enter_multithreaded();
                     // This is the emulator's equivalent of creating a
                     // guest pthread: tell glibc not to use its single-thread
                     // fast paths. Store exactly the exported char byte.
                     if (libc_single_threaded_addr_)
                         mem_.store<uint8_t>(libc_single_threaded_addr_, 0);
                     return cpu;
                 });
    // Probe after installing the runner and TLS-aware guest CPU factory.
    // start_pump() waits until a callback stream is open, so guests that do
    // not use callback audio keep the single-vCPU JIT path.
    athunk->start_pump();
}

// ── wire_thunk_wl_cb_runner_ — Wayland listener delivery ──────────
// wl_proxy_add_listener stores guest callbacks; host events queue during
// dispatch and deliver here through the shared borrow-CPU helper.
void Emulator::wire_thunk_wl_cb_runner_() {
    auto* dthunk = graphics_.display_thunk();
    if (!dthunk || !dthunk->enabled()) return;
    dthunk->set_wl_cb_runner(
        [this](CPU& cpu, uint64_t fn, const int64_t* iargs,
               size_t n) -> uint64_t {
            return call_guest_function(cpu, fn, iargs, n, nullptr, 0);
        });
}

// ── wire_thunk_wl_fd_publisher_ — host fd -> guest fd ────────────
// inbound wayland fds (keymap, data send) arrive as host fds from the
// compositor. dup (the compositor keeps its copy; HostNode closes ours)
// then wrap + allocate a guest fd so the guest can mmap/read it.
void Emulator::wire_thunk_wl_fd_publisher_() {
    auto* dthunk = graphics_.display_thunk();
    if (!dthunk || !dthunk->enabled()) return;
    dthunk->set_wl_fd_publisher([this](int host_fd) -> int {
        if (host_fd < 0) return -1;
        int duped = ::dup(host_fd);
        if (duped < 0) return -1;
        auto node = std::make_shared<yggdrasil::HostNode>(duped, O_RDONLY);
        int gfd = fds_.allocate(node);
        if (gfd < 0) ::close(duped);
        return gfd;
    });
}

void Emulator::ensure_thunk_linker_() {
    if (dyn_linker_) return;
    dyn_linker_ = std::make_unique<DynamicLinker>(mem_);
    if (auto* t = graphics_.thunk()) {
        if (t->enabled()) {
            t->init(mem_);
            wire_thunk_glfw_cb_runner_();
            wire_thunk_sdl_thread_runner_();
        }
    }
    if (auto* at = graphics_.audio_thunk()) {
        if (at->enabled()) { at->init(mem_); wire_thunk_audio_runner_(); }
    }
    if (auto* dt = graphics_.display_thunk()) {
        if (dt->enabled()) {
            dt->init(mem_);
            wire_thunk_wl_cb_runner_();
            wire_thunk_wl_fd_publisher_();
            dt->set_wl_fd_resolver([this](int gfd) -> int {
                auto n = fds_.get(gfd);
                if (!n) return -1;
                return n->host_fd();
            });
        }
    }
    GraphicThunk* gthunk = graphics_.thunk();
    AudioThunk* athunk = graphics_.audio_thunk();
    DisplayThunk* dthunk = graphics_.display_thunk();
    dyn_linker_->set_thunk_resolver(
        [gthunk, athunk, dthunk](const std::string& lib)
            -> DynamicLinker::ThunkSymbolList {
            DynamicLinker::ThunkSymbolList out;
            auto add_all = [&](auto* t) {
                if (!t || !t->enabled()) return;
                t->enumerate_symbols(lib, [&](const std::string& s, uint64_t a) {
                    out.emplace_back(s, a);
                });
            };
            add_all(gthunk);
            add_all(athunk);
            add_all(dthunk);
            return out;
        });
    dyn_linker_->set_ifunc_resolver([this](CPU& cpu, uint64_t a) -> uint64_t {
        if (a == 0) return 0;
        struct SavedState { uint64_t regs[32]; uint64_t sp, pc; uint32_t pstate; uint64_t v_lo[32], v_hi[32]; uint32_t fpcr, fpsr; uint64_t tpidr_el0, tpidrro_el0; uint64_t sigmask; bool running; } s{};
        std::memcpy(s.regs, cpu.regs, sizeof(s.regs)); s.sp=cpu.sp; s.pc=cpu.pc; s.pstate=cpu.pstate;
        std::memcpy(s.v_lo, cpu.v_lo, sizeof(s.v_lo)); std::memcpy(s.v_hi, cpu.v_hi, sizeof(s.v_hi));
        s.fpcr=cpu.fpcr; s.fpsr=cpu.fpsr; s.tpidr_el0=cpu.tpidr_el0; s.tpidrro_el0=cpu.tpidrro_el0; s.sigmask=cpu.sigmask; s.running=cpu.running;
        auto restore=[&](){ std::memcpy(cpu.regs, s.regs, sizeof(s.regs)); cpu.sp=s.sp; cpu.pc=s.pc; cpu.pstate=s.pstate; std::memcpy(cpu.v_lo, s.v_lo, sizeof(s.v_lo)); std::memcpy(cpu.v_hi, s.v_hi, sizeof(s.v_hi)); cpu.fpcr=s.fpcr; cpu.fpsr=s.fpsr; cpu.tpidr_el0=s.tpidr_el0; cpu.tpidrro_el0=s.tpidrro_el0; cpu.sigmask=s.sigmask; cpu.running=s.running; };
        static thread_local uint64_t scratch=0; static thread_local const Emulator* scratch_owner=nullptr;
        if (scratch==0 || scratch_owner!=this) { scratch=mem_.mmap_alloc_callback_stack(64*1024); scratch_owner=this; if(scratch==0){ restore(); return 0; } }
        uint64_t top=scratch+64*1024; constexpr uint64_t SR=0x1000;
        cpu.pc=a; cpu.sp=top; cpu.regs[30]=SR; cpu.running=true; cpu.pstate=0;
        constexpr uint64_t LIM=1'000'000; uint64_t steps=0;
        try { while(cpu.running && cpu.pc!=SR && steps<LIM){ step(cpu); steps++; } } catch(...) {}
        // Limit tripped → likely infinite resolver: return 0 like the
        // primary wiring does, never a mid-execution register value.
        uint64_t result = (steps >= LIM) ? 0 : cpu.regs[0];
        restore(); return result;
    });
    dyn_linker_->set_init_runner([this](CPU& cpu, uint64_t fn) {
        if(!fn) return;
        struct SavedState { uint64_t regs[32]; uint64_t sp, pc; uint32_t pstate; uint64_t v_lo[32], v_hi[32]; uint32_t fpcr, fpsr; uint64_t tpidr_el0, tpidrro_el0; uint64_t sigmask; bool running; } s{};
        std::memcpy(s.regs, cpu.regs, sizeof(s.regs)); s.sp=cpu.sp; s.pc=cpu.pc; s.pstate=cpu.pstate;
        std::memcpy(s.v_lo, cpu.v_lo, sizeof(s.v_lo)); std::memcpy(s.v_hi, cpu.v_hi, sizeof(s.v_hi));
        s.fpcr=cpu.fpcr; s.fpsr=cpu.fpsr; s.tpidr_el0=cpu.tpidr_el0; s.tpidrro_el0=cpu.tpidrro_el0; s.sigmask=cpu.sigmask; s.running=cpu.running;
        auto restore=[&](){ std::memcpy(cpu.regs, s.regs, sizeof(s.regs)); cpu.sp=s.sp; cpu.pc=s.pc; cpu.pstate=s.pstate; std::memcpy(cpu.v_lo, s.v_lo, sizeof(s.v_lo)); std::memcpy(cpu.v_hi, s.v_hi, sizeof(s.v_hi)); cpu.fpcr=s.fpcr; cpu.fpsr=s.fpsr; cpu.tpidr_el0=s.tpidr_el0; cpu.tpidrro_el0=s.tpidrro_el0; cpu.sigmask=s.sigmask; cpu.running=s.running; };
        if(dyn_linker_ && dyn_linker_->static_tls_size()>0 && cpu.tpidr_el0==0){ cpu.tpidr_el0=dyn_linker_->thread_pointer(); cpu.tpidrro_el0=cpu.tpidr_el0; }
        static thread_local uint64_t scratch=0; static thread_local const Emulator* scratch_owner=nullptr;
        if (scratch==0 || scratch_owner!=this) { scratch=mem_.mmap_alloc_callback_stack(64*1024); scratch_owner=this; if(scratch==0){ restore(); return; } }
        uint64_t top=scratch+64*1024; constexpr uint64_t SR=0x1000;
        cpu.pc=fn; cpu.sp=top; cpu.regs[30]=SR; cpu.running=true; cpu.pstate=0;
        constexpr uint64_t LIM=10'000'000; uint64_t steps=0;
        try { while(cpu.running && cpu.pc!=SR && steps<LIM){ step(cpu); steps++; } } catch(const std::exception& e){ if(dbg().dynlink_trace) fprintf(stderr,"[%s] init 0x%llx: %s\n", CODENAME, (unsigned long long)fn, e.what()); }
        restore();
    });
    dyn_linker_->set_guest_call_args([this](CPU& cpu, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2) -> uint64_t {
        struct SavedState { uint64_t regs[32]; uint64_t sp, pc; uint32_t pstate; uint64_t v_lo[32], v_hi[32]; uint32_t fpcr, fpsr; uint64_t tpidr_el0, tpidrro_el0; uint64_t sigmask; bool running; } s{};
        std::memcpy(s.regs, cpu.regs, sizeof(s.regs)); s.sp=cpu.sp; s.pc=cpu.pc; s.pstate=cpu.pstate;
        std::memcpy(s.v_lo, cpu.v_lo, sizeof(s.v_lo)); std::memcpy(s.v_hi, cpu.v_hi, sizeof(s.v_hi));
        s.fpcr=cpu.fpcr; s.fpsr=cpu.fpsr; s.tpidr_el0=cpu.tpidr_el0; s.tpidrro_el0=cpu.tpidrro_el0; s.sigmask=cpu.sigmask; s.running=cpu.running;
        auto restore=[&](){ std::memcpy(cpu.regs, s.regs, sizeof(s.regs)); cpu.sp=s.sp; cpu.pc=s.pc; cpu.pstate=s.pstate; std::memcpy(cpu.v_lo, s.v_lo, sizeof(s.v_lo)); std::memcpy(cpu.v_hi, s.v_hi, sizeof(s.v_hi)); cpu.fpcr=s.fpcr; cpu.fpsr=s.fpsr; cpu.tpidr_el0=s.tpidr_el0; cpu.tpidrro_el0=s.tpidrro_el0; cpu.sigmask=s.sigmask; cpu.running=s.running; };
        if(dyn_linker_ && dyn_linker_->static_tls_size()>0 && cpu.tpidr_el0==0){ cpu.tpidr_el0=dyn_linker_->thread_pointer(); cpu.tpidrro_el0=cpu.tpidr_el0; }
        static thread_local uint64_t ss=0; static thread_local const Emulator* ss_owner=nullptr;
        if(!ss || ss_owner!=this){ ss=mem_.mmap_alloc_callback_stack(64*1024); ss_owner=this; if(!ss){ restore(); return 0; } }
        uint64_t top=ss+64*1024; constexpr uint64_t SR=0x1000;
        cpu.pc=fn; cpu.sp=top; cpu.regs[0]=a0; cpu.regs[1]=a1; cpu.regs[2]=a2; cpu.regs[30]=SR; cpu.running=true; cpu.pstate=0;
        constexpr uint64_t LIM=50'000'000; uint64_t steps=0;
        try { while(cpu.running && cpu.pc!=SR && steps<LIM){ step(cpu); steps++; } } catch(...) {}
        uint64_t r=cpu.regs[0]; restore(); return r;
    });
    wire_thunk_android_runner_();
}
// ── call_guest_function — borrow-CPU guest function call (C API) ───────
// libbifrost's bifrost_call() lets embedders invoke an
// arbitrary guest function (e.g. a callback, an exported entry, or a
// helper in a loaded library) and read back x0. Same borrow-CPU pattern
// as wire_thunk_glfw_cb_runner_ / guest_call_args_: save/restore ALL
// architectural state, set x0..x7 and d0..d7 from the argument arrays,
// pc = fn, LR = sentinel (0x1000), step() until the callee RETs (or a
// 50M-instruction limit), restore, return x0.
uint64_t Emulator::call_guest_function(CPU& cpu, uint64_t fn,
                                       const int64_t* iargs, size_t n_iargs,
                                       const double* fargs, size_t n_fargs,
                                       double* fp_result) {
    if (fn == 0) return 0;
    struct SavedState {
        uint64_t regs[32];
        uint64_t sp, pc;
        uint32_t pstate;
        uint64_t v_lo[32], v_hi[32];
        uint32_t fpcr, fpsr;
        uint64_t tpidr_el0, tpidrro_el0;
        uint64_t sigmask;
        bool running;
    } saved;
    std::memcpy(saved.regs, cpu.regs, sizeof(saved.regs));
    saved.sp = cpu.sp;
    saved.pc = cpu.pc;
    saved.pstate = cpu.pstate;
    std::memcpy(saved.v_lo, cpu.v_lo, sizeof(saved.v_lo));
    std::memcpy(saved.v_hi, cpu.v_hi, sizeof(saved.v_hi));
    saved.fpcr = cpu.fpcr;
    saved.fpsr = cpu.fpsr;
    saved.tpidr_el0 = cpu.tpidr_el0;
    saved.tpidrro_el0 = cpu.tpidrro_el0;
    saved.sigmask = cpu.sigmask;
    saved.running = cpu.running;
    auto restore = [&]() {
        std::memcpy(cpu.regs, saved.regs, sizeof(saved.regs));
        cpu.sp = saved.sp;
        cpu.pc = saved.pc;
        cpu.pstate = saved.pstate;
        std::memcpy(cpu.v_lo, saved.v_lo, sizeof(saved.v_lo));
        std::memcpy(cpu.v_hi, saved.v_hi, sizeof(saved.v_hi));
        cpu.fpcr = saved.fpcr;
        cpu.fpsr = saved.fpsr;
        cpu.tpidr_el0 = saved.tpidr_el0;
        cpu.tpidrro_el0 = saved.tpidrro_el0;
        cpu.sigmask = saved.sigmask;
        cpu.running = saved.running;
    };
    if (dyn_linker_ && dyn_linker_->static_tls_size() > 0 &&
        cpu.tpidr_el0 == 0) {
        cpu.tpidr_el0 = dyn_linker_->thread_pointer();
        cpu.tpidrro_el0 = cpu.tpidr_el0;
    }
    // Scratch stack (thread-local so concurrent guest threads can't
    // clobber each other's callee frame).
    static thread_local uint64_t scratch_stack = 0;
    static thread_local const Emulator* scratch_owner = nullptr;
    // 64 KiB usable + 4 KiB head margin (2026-08-25): the old 8 KiB
    // scratch sat MID-HEAP next to live malloc chunks. Host-thread
    // callbacks (the audio pump's cloned vCPU firing SDL audio mixers)
    // need more guest stack than 8 KiB; the overflow descended into the
    // NEIGHBORING CHUNKS and shredded glibc's heap ("malloc(): invalid
    // size" in neverball, AllocBlock chunk shredding in vkQuake). The
    // corruption is invisible to every verification mode because it
    // comes from a host-side thread, never from a verified guest block.
    constexpr uint64_t CB_SCRATCH_USABLE = 128 * 1024;
    if (scratch_stack == 0 || scratch_owner != this) {
        // Guarded isolated layout: [PROT_NONE guard][usable][guard] —
        // overflow faults loudly instead of trashing live chunks.
        // Returns the usable base (guard page below already skipped).
        scratch_stack = mem_.mmap_alloc_callback_stack(CB_SCRATCH_USABLE);
        if (scratch_stack == 0) return 0;
        scratch_owner = this;
        if (getenv("BIFROST_SCRATCH_TRACE"))
            fprintf(stderr, "[scratch] cpu=%p usable=[0x%llx..0x%llx)\n",
                    (void*)&cpu,
                    (unsigned long long)scratch_stack,
                    (unsigned long long)(scratch_stack + CB_SCRATCH_USABLE));
    }
    uint64_t stack_top = scratch_stack + CB_SCRATCH_USABLE;
    constexpr uint64_t SENTINEL_LR = 0x1000;
    cpu.pc = fn;
    // AAPCS64: x0..x7 in regs, the rest on the stack at sp (16-aligned).
    // Wayland geometry listeners need 10 words (data, proxy + 8); the
    // old clamp silently dropped arg9+ and the callee read stack garbage
    // (host SIGSEGV via a window-aliased raw read, not a catchable
    // guest fault).
    size_t n_stack = (n_iargs > 8) ? n_iargs - 8 : 0;
    if (n_stack) {
        size_t bytes = (n_stack * 8 + 15) & ~size_t(15);
        stack_top -= bytes;
        for (size_t i = 0; i < n_stack; i++) {
            uint64_t v = static_cast<uint64_t>(iargs[8 + i]);
            mem_.write(stack_top + i * 8, &v, sizeof(v));
        }
    }
    cpu.sp = stack_top;
    if (n_iargs > 8) n_iargs = 8;
    if (n_fargs > 8) n_fargs = 8;
    for (size_t i = 0; i < n_iargs; i++)
        cpu.regs[i] = static_cast<uint64_t>(iargs[i]);
    for (size_t i = 0; i < n_fargs; i++) {
        std::memcpy(&cpu.v_lo[i], &fargs[i], sizeof(fargs[i]));
        cpu.v_hi[i] = 0;
    }
    cpu.regs[30] = SENTINEL_LR;
    cpu.running = true;
    cpu.pstate = 0;
    constexpr uint64_t CALL_LIMIT = 50'000'000;
    uint64_t steps = 0;
    uint64_t result = 0;
    try {
        while (cpu.running && cpu.pc != SENTINEL_LR && steps < CALL_LIMIT) {
            step(cpu);
            steps++;
        }
        result = cpu.regs[0];
        if (fp_result) {
            std::memcpy(fp_result, &cpu.v_lo[0], sizeof(double));
        }
    } catch (const std::exception&) {
        result = 0;
        if (fp_result) *fp_result = 0.0;
    }
    restore();
    return result;
}
// ── wire_thunk_sdl_thread_runner_ — SDL_CreateThread/WaitThread ──────
// SDL_CreateThread passes a guest AArch64 function pointer;
// host SDL_CreateThread would run it as x86-64 (SIGSEGV). Instead we
// spawn a REAL guest thread (own CPU + host thread + fresh stack +
// per-thread glibc TLS) running the guest function, and SDL_WaitThread
// joins it. The guest SDL_Thread* handle is a guest-addressable struct
// whose done-word the wait blocks on (see Threads.cpp).
void Emulator::wire_thunk_sdl_thread_runner_() {
    auto* thunk = graphics_.thunk();
    if (!thunk || !thunk->enabled()) return;
    thunk->set_sdl_thread_runner(
        [this](CPU& cpu, uint32_t op, uint64_t a0, uint64_t a1,
               uint64_t a2) -> uint64_t {
            (void)cpu;
            if (op == 0) {
                // create: a0=fn, a1=name (unused), a2=data
                return spawn_sdl_thread(a0, a2);
            }
            if (op == 1) {
                // wait: a0=thread handle, a1=status ptr (may be 0)
                wait_sdl_thread(a0, a1);
                return 0;
            }
            if (op == 2) {
                // detach: a0=thread handle
                detach_sdl_thread(a0);
                return 0;
            }
            if (op == 3) {
                std::lock_guard<std::mutex> lock(sdl_threads_mu_);
                auto it = sdl_threads_.find(a0);
                return it == sdl_threads_.end() ? 0 : it->second->tid;
            }
            return 0;
        });
}
}  // namespace arm64emu
