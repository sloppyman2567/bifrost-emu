// Android activity loading and the Android-profile run loop.
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
void Emulator::load_android_activity(const std::string& path,
                                     std::vector<std::string>& argv) {
    mem_.map_range(0, Memory::PAGE_SIZE);
    ensure_thunk_linker_();
    uint64_t handle = dyn_linker_->load_library(main_cpu_, path);
    if (handle == 0) {
        throw std::runtime_error(std::string("load_android_activity: ") +
                                 dyn_linker_->error());
    }
    uint64_t oc = dyn_linker_->resolve_symbol_in(handle, "ANativeActivity_onCreate");
    if (oc == 0) {
        throw std::runtime_error("load_android_activity: no ANativeActivity_onCreate in " + path);
    }
    android_on_create_ = oc;

    // Stack / env / TLS / vDSO — mirror load_elf_file tail.
    const uint64_t STACK_TOP = mem_.stack_top();
    const uint64_t STACK_SIZE = Memory::STACK_SIZE;
    uint64_t stack_base = STACK_TOP - STACK_SIZE;
    mem_.map_range(stack_base, STACK_SIZE + 4096);
    load_vdso();
    // Dummy LoadInfo for auxv (no real PHDR).
    ElfLoader::Loaded dummy{};
    dummy.entry = 0; dummy.phdr_addr = 0; dummy.phnum = 0; dummy.phent = 0;
    main_cpu_.sp = build_initial_stack(STACK_TOP, argv, dummy);
    if (dyn_linker_) dyn_linker_->set_guest_environ(guest_envp_addr_);
    if (!dyn_linker_ || dyn_linker_->static_tls_size() == 0) {
        if (main_cpu_.tpidr_el0 == 0) {
            const uint64_t TLS_SCRATCH_SIZE = 65536;
            uint64_t tls = mem_.mmap_alloc(TLS_SCRATCH_SIZE);
            main_cpu_.tpidr_el0 = tls + TLS_SCRATCH_SIZE / 2;
            main_cpu_.tpidrro_el0 = main_cpu_.tpidr_el0;
        }
    } else {
        if (main_cpu_.tpidr_el0 == 0) {
            uint64_t tp = dyn_linker_->thread_pointer();
            main_cpu_.tpidr_el0 = tp;
            main_cpu_.tpidrro_el0 = tp;
        }
    }
    // Brk: one heap page so sbrk/brk consults a valid base.
    if (brk_ == 0) {
        uint64_t hp = mem_.mmap_alloc(4096);
        brk_ = hp + 4096;
        brk_start_ = brk_;
    }
    mem_.map_range(0, 4096);
    main_cpu_.tid = vfs_.self_pid(); next_tid_ = 2;
    main_cpu_.set_tid_address_ptr = 0;
    main_cpu_.pc = 0;
    main_cpu_.running = false;

    auto& amgr = frost::AndroidSurfaceManager::instance();
    wire_thunk_android_runner_();
    // Create activity structures eagerly so onCreate can fill callbacks.
    amgr.create_activity();
    // Eager window so from_surface works before any guest call.
    if (auto* dt = graphics_.display_thunk()) dt->ensure_android_window();
}

int Emulator::run_android() {
    auto& amgr = frost::AndroidSurfaceManager::instance();
    if (android_on_create_ == 0 || amgr.activity_addr() == 0) return 1;
    wire_thunk_android_runner_();
    if (auto* dt = graphics_.display_thunk()) dt->ensure_android_window();

    // Fire ANativeActivity_onCreate(activity, NULL, 0).
    {
        int64_t args[3] = {static_cast<int64_t>(amgr.activity_addr()), 0, 0};
        call_guest_function(main_cpu_, android_on_create_, args, 3, nullptr, 0);
    }

    // Lifecycle: mirrors framework order. Each fire reads the guest
    // callback table live — native_app_glue fills its entries inside
    // onCreate, so later fires dispatch to the glue.
    amgr.fire_activity_cb(main_cpu_, frost::AndroidSurfaceManager::CB_ON_START);
    amgr.fire_activity_cb(main_cpu_, frost::AndroidSurfaceManager::CB_ON_RESUME);
    {
        int64_t one = 1;
        amgr.fire_activity_cb(main_cpu_,
                              frost::AndroidSurfaceManager::CB_ON_WINDOW_FOCUS, &one, 1);
    }
    // Input queue before window (order tolerant either way).
    uint64_t qh = amgr.input_queue_handle();
    {
        int64_t qa = static_cast<int64_t>(qh);
        amgr.fire_activity_cb(main_cpu_,
                              frost::AndroidSurfaceManager::CB_ON_INPUT_QUEUE_CREATED,
                              &qa, 1);
    }
    uint64_t wh = amgr.from_surface();
    if (wh) {
        int64_t wa = static_cast<int64_t>(wh);
        amgr.fire_activity_cb(main_cpu_,
                              frost::AndroidSurfaceManager::CB_ON_NATIVE_WINDOW_CREATED,
                              &wa, 1);
        // Test hook: one synthetic tap right after the window exists so
        // the full input path (queue → getEvent → getters) is exercised
        // without a human moving the mouse.
        const char* tap = getenv("BIFROST_ANDROID_TAP");
        if (tap && tap[0] != '0' && tap[0] != '\0') {
            amgr.inject_test_tap();
        }
    }

    // Hard wall-clock cap, OPT-IN via BIFROST_ANDROID_TIMEOUT_SECS=N
    // (CI safety). Unset/0 = pump until the window closes or a guest
    // exit_group tears the whole process down.
    int64_t cap_secs = 0;
    if (const char* t = getenv("BIFROST_ANDROID_TIMEOUT_SECS")) {
        cap_secs = strtoll(t, nullptr, 10);
    }
    const auto hard_deadline =
        cap_secs > 0
            ? std::chrono::steady_clock::now() +
                  std::chrono::seconds(cap_secs)
            : std::chrono::steady_clock::time_point::max();

    while (!amgr.quit_requested()) {
        if (std::chrono::steady_clock::now() > hard_deadline) {
            std::fprintf(stderr, "[android] hard timeout\n");
            break;
        }
        amgr.pump_host_events(&main_cpu_);
        // A guest exit_group from any thread terminates the host process
        // directly; there is nothing to detect here.
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    // Graceful shutdown (no-op if the process already exited).
    amgr.fire_activity_cb(main_cpu_, frost::AndroidSurfaceManager::CB_ON_PAUSE);
    amgr.fire_activity_cb(main_cpu_, frost::AndroidSurfaceManager::CB_ON_STOP);
    {
        int64_t qa = static_cast<int64_t>(qh);
        amgr.fire_activity_cb(main_cpu_,
                              frost::AndroidSurfaceManager::CB_ON_INPUT_QUEUE_DESTROYED,
                              &qa, 1);
    }
    if (wh) {
        int64_t wa = static_cast<int64_t>(wh);
        amgr.fire_activity_cb(main_cpu_,
                              frost::AndroidSurfaceManager::CB_ON_NATIVE_WINDOW_DESTROYED,
                              &wa, 1);
    }
    amgr.fire_activity_cb(main_cpu_, frost::AndroidSurfaceManager::CB_ON_DESTROY);
    return 0;
}
}  // namespace arm64emu
