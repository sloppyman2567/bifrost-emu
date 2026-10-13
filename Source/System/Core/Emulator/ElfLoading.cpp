// ElfLoading.cpp — Emulator lifecycle, ELF loading, run loop.
//
// This file implements the high-level "drive the emulator" logic:
//   - Constructor / destructor
//   - load_elf_file() — load an ELF, set up the initial stack + TLS
//   - run() — main interpreter/JIT dispatch loop with hang watchdog,
//     periodic signal draining, and SDL2 refresh hooks
//   - step() — decode-cache-aware single-instruction step (with tracing)
//   - Host signal handlers (SIGINT/SIGTERM/SIGCHLD/SIGWINCH → guest)
//
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
void* Emulator::excl_monitor_shard_pub(uint64_t addr) {
    return &excl_monitor_shards_[excl_shard_idx(addr)];
}
// ── ELF loading ───────────────────────────────────────────────────────
void Emulator::load_elf_file(const std::string& path, std::vector<std::string>& argv) {
    if (android_runtime_ && (!yggdrasil::Yggdrasil::sandbox_inside_root() ||
                            path.empty() || path.front() != '/'))
        throw EmuError("Android runtime bootstrap requires --rootfs and an absolute guest ELF path");
    // Read Android runtime images through the same confined VFS as guest opens.
    auto read_android_elf = [this](const std::string& guest_path) {
        int err = 0;
        auto node = vfs_.open(guest_path, O_RDONLY, 0, &err);
        if (!node) throw EmuError("cannot open Android runtime ELF " + guest_path +
                                  ": " + strerror(err < 0 ? -err : ENOENT));
        struct stat st{};
        if (node->fstat(&st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
            st.st_size > 512LL * 1024 * 1024)
            throw EmuError("invalid or oversized Android runtime ELF " + guest_path);
        std::vector<uint8_t> bytes(static_cast<size_t>(st.st_size));
        size_t offset = 0;
        while (offset < bytes.size()) {
            ssize_t n = node->read(offset, bytes.data() + offset, bytes.size() - offset);
            if (n == -EINTR) continue;
            if (n <= 0) throw EmuError("short read of Android runtime ELF " + guest_path);
            offset += static_cast<size_t>(n);
        }
        return bytes;
    };
    // The loader copies ELF bytes; name its PT_LOAD pages in /proc/self/maps
    // after the file, as the kernel names its own mappings (resolved path).
    auto name_segments = [this](const ElfLoader::Loaded& image, const std::string& guest_path) {
        Memory::FileIdentity file;
        file.path = guest_path;
        int err = 0;
        if (auto node = vfs_.open(guest_path, O_RDONLY, 0, &err)) {
            struct stat st{};
            if (node->fstat(&st) == 0) {
                file.dev = st.st_dev;
                file.inode = st.st_ino;
            }
            std::string resolved;
            if (node->host_fd() >= 0 &&
                yggdrasil::Yggdrasil::fd_link_target(node->host_fd(), resolved) == 0)
                file.path = resolved;
        }
        for (const auto& segment : image.segments)
            mem_.name_image(segment.addr, segment.file_end - segment.addr, segment.offset, file);
    };
    // IFUNC resolvers can read Linux auxiliary data at low addresses while
    // the dynamic linker is running. Keep the emulator's zero page mapped
    // before loading or relocating any guest objects. The Android profile
    // leaves it unmapped like Linux (mmap_min_addr): ART's implicit null
    // checks need the fault to raise NullPointerException, and Bionic's
    // linker passes resolvers a real argument block.
    if (!android_runtime_) mem_.map_range(0, Memory::PAGE_SIZE);
    elf_path_ = path;
    // Initialize the guest process name (comm) to the ELF basename.
    // This matches the Linux kernel behavior: the initial comm is the
    // executable's basename, truncated to 15 chars. prctl(PR_SET_NAME)
    // can override it later.
    {
        size_t slash = path.find_last_of('/');
        std::string base = (slash != std::string::npos) ? path.substr(slash + 1) : path;
        set_guest_comm(base);
    }
    vfs_.set_elf_path(path);
    // /proc/self/cmdline follows Linux get_mm_cmdline (fs/proc/base.c):
    // the argv block as it is now, or, when the guest overwrote its final
    // NUL (setproctitle), the string at arg_start_ within one page.
    vfs_.set_cmdline_provider([this]() -> std::string {
        std::string block;
        if (arg_start_ >= arg_end_) return block;
        try {
            uint8_t last = mem_.load<uint8_t>(arg_end_ - 1);
            const uint64_t length = last ? std::min<uint64_t>(Memory::PAGE_SIZE, arg_end_ - arg_start_)
                                         : arg_end_ - arg_start_;
            block.resize(length);
            mem_.read(arg_start_, block.data(), length);
            if (last) {
                size_t nul = block.find('\0');
                if (nul != std::string::npos) block.resize(nul + 1);
            }
        } catch (const EmuError&) {
            block.clear();
        }
        return block;
    });
    vfs_.set_graphics(&graphics_);
    vfs_.set_audio(&audio_);
    // /proc/self/maps lists every guest mapping as the kernel does: real
    // permissions, and file offset, device, inode and resolved path for
    // file-backed pages. The callback captures `this`; the Emulator outlives
    // the VFS. Bionic and glibc find the main stack as the first line that
    // contains the initial SP, so [stack] is split out of coalesced ranges.
    vfs_.set_maps_provider([this]() {
        struct Special { uint64_t start, end; const char* name; };
        std::vector<Special> specials;
        if (brk_start_ > 0 && brk_ > brk_start_)  // Linux omits an empty [heap]
            specials.push_back({brk_start_, brk_, "[heap]"});
        specials.push_back({mem_.stack_top() - Memory::STACK_SIZE, mem_.stack_top(), "[stack]"});
        if (vdso_base_ && vdso_size_)
            specials.push_back({vdso_base_, vdso_base_ + vdso_size_, "[vdso]"});
        std::vector<yggdrasil::Yggdrasil::MapEntry> out;
        for (const auto& view : mem_.mappings_snapshot()) {
            std::vector<uint64_t> cuts{view.start, view.end};
            if (!view.file)
                for (const auto& special : specials)
                    for (uint64_t at : {special.start, special.end})
                        if (at > view.start && at < view.end) cuts.push_back(at);
            std::sort(cuts.begin(), cuts.end());
            for (size_t i = 0; i + 1 < cuts.size(); ++i) {
                yggdrasil::Yggdrasil::MapEntry e;
                e.start = cuts[i];
                e.end = cuts[i + 1];
                e.perms[0] = (view.prot & Memory::GUEST_PROT_READ) ? 'r' : '-';
                e.perms[1] = (view.prot & Memory::GUEST_PROT_WRITE) ? 'w' : '-';
                e.perms[2] = (view.prot & Memory::GUEST_PROT_EXEC) ? 'x' : '-';
                e.perms[3] = view.shared ? 's' : 'p';
                e.perms[4] = '\0';
                if (view.file) {
                    e.offset = view.offset + (e.start - view.start);
                    e.dev = view.file->dev;
                    e.inode = view.file->inode;
                    e.label = view.file->host_path
                        ? yggdrasil::Yggdrasil::guest_view_of_host_path(view.file->path)
                        : view.file->path;
                } else {
                    for (const auto& special : specials)
                        if (special.start <= e.start && e.end <= special.end) e.label = special.name;
                }
                out.push_back(std::move(e));
            }
        }
        return out;
    });
    // Wire up guest cwd tracking. The host cwd is meaningless because
    // BIFROST_ROOT sandboxing decouples guest paths from host paths.
    // The guest starts at "/" by default; chdir/fchdir update this.
    vfs_.set_cwd_provider(
        [this]() -> std::string { return guest_cwd_; },
        [this](const std::string& p) -> bool {
            // Resolve relative paths against the current cwd.
            if (p.empty()) return false;
            if (p[0] == '/') {
                guest_cwd_ = p;
            } else {
                if (guest_cwd_.empty()) guest_cwd_ = "/";
                if (guest_cwd_.back() == '/') guest_cwd_.pop_back();
                guest_cwd_ += "/";
                guest_cwd_ += p;
            }
            // Normalize: collapse "." and ".." segments.
            std::vector<std::string> parts;
            std::stringstream ss(guest_cwd_);
            std::string seg;
            while (std::getline(ss, seg, '/')) {
                if (seg.empty() || seg == ".") continue;
                if (seg == "..") {
                    if (!parts.empty()) parts.pop_back();
                } else {
                    parts.push_back(seg);
                }
            }
            guest_cwd_ = "/";
            for (size_t i = 0; i < parts.size(); i++) {
                guest_cwd_ += parts[i];
                if (i + 1 < parts.size()) guest_cwd_ += "/";
            }
            return true;
        });
    // Wire up /proc/self/comm — returns the guest process name (set by
    // prctl PR_SET_NAME, defaults to ELF basename).
    vfs_.set_comm_provider([this]() -> std::string {
        return guest_comm_;
    });
    // Open the ELF through the BIFROST_ROOT sandbox: `path` is the guest
    // path (what /proc/self/exe reports), so the host file is the remapped
    // "$BIFROST_ROOT/<path>". Relative paths pass through unchanged, so
    // existing `bifrost-emu Tests/Unit/foo.elf` invocations are untouched.
    // This keeps /proc/self/exe consistent with the guest's view of the
    // filesystem (Qt derives applicationDirPath from it).
    const std::string host_elf_path = yggdrasil::Yggdrasil::remap_path(path);
    FILE* f = android_runtime_ ? nullptr : fopen(host_elf_path.c_str(), "rb");
    if (!android_runtime_ && !f)
        throw EmuError("cannot open " + host_elf_path + ": " + strerror(errno));
    // Linux /proc/self/exe is absolute even when argv[0] is relative.
    // Keep argv/AT_EXECFN unchanged, and strip the sandbox's host prefix.
    if (char* resolved = ::realpath(host_elf_path.c_str(), nullptr)) {
        const std::string backing_path = resolved;
        elf_path_ = resolved;
        free(resolved);
        const std::string mapped_root = yggdrasil::Yggdrasil::remap_path("/");
        if (mapped_root != "/") {
            if (char* root = ::realpath(mapped_root.c_str(), nullptr)) {
                const std::string prefix = std::string(root) == "/" ? "/" : std::string(root) + "/";
                if (elf_path_.compare(0, prefix.size(), prefix) == 0)
                    elf_path_ = elf_path_.substr(prefix.size() - 1);
                else
                    elf_path_ = path.front() == '/' ? path : "/" + path;
                free(root);
            }
        }
        vfs_.set_elf_path(elf_path_, backing_path);
    }

    std::vector<uint8_t> data;
    if (android_runtime_) {
        data = read_android_elf(path);
    } else {
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); throw EmuError("empty or invalid ELF"); }
    data.resize(sz);
    if (fread(data.data(), 1, sz, f) != static_cast<size_t>(sz)) {
        fclose(f); throw EmuError("short read on " + host_elf_path);
    }
    fclose(f);
    }
    ElfLoader::Options load_options;
    load_options.apply_relocations = !android_runtime_;
    auto info = ElfLoader::load(mem_, data, load_options);
    name_segments(info, path);
    entry_ = info.entry;
    prog_entry_ = info.entry;  // save original entry for AT_ENTRY
    end_addr_ = info.end_addr;
    phdr_addr_ = info.phdr_addr;
    phnum_ = info.phnum;
    phent_ = info.phent;
    has_lse_ = info.has_lse;
    interp_base_ = 0;
    // If the binary has a PT_INTERP (dynamic linker), load it.
    // The dynamic linker's entry point becomes the real entry point;
    // the binary's entry is passed via AT_ENTRY in auxv.
    //
    // Two paths are supported:
    //   (a) Default: our own DynamicLinker processes DT_NEEDED, applies
    //       relocations, and resolves symbols — no guest-side ld.so
    //       needed. Faster and works even when the host doesn't have the
    //       exact aarch64 ld.so.
    //   (b) With BIFROST_NO_NATIVE_DYNLINK=1, load the guest-side dynamic
    //       linker (ld-musl / ld-linux) and let it run its own code
    //       (debug path).
    uint64_t interp_base = 0;
    if (android_runtime_) {
        if (info.interp != "/system/bin/linker64" &&
            info.interp != "/apex/com.android.runtime/bin/linker64")
            throw EmuError("Android runtime bootstrap requires a Bionic linker64 PT_INTERP");
        if (!info.phdr_addr)
            throw EmuError("Android runtime ELF has no mapped program-header table");
        auto interpreter = read_android_elf(info.interp);
        ElfLoader::Options interpreter_options;
        interpreter_options.apply_relocations = false;
        interpreter_options.reserve_image = true;
        auto loaded = ElfLoader::load(mem_, interpreter, interpreter_options);
        if (!loaded.interp.empty())
            throw EmuError("Android linker64 must not itself require PT_INTERP");
        name_segments(loaded, info.interp);
        dyn_linker_.reset();
        entry_ = loaded.entry;
        interp_base_ = loaded.base_addr;
        has_lse_ = has_lse_ || loaded.has_lse;
        // Guest shim DSOs resolve only explicit adapter exports. Bionic
        // continues to own all ELF relocation/TLS; no custom linker is made.
        if (auto* thunk = graphics_.thunk(); thunk && thunk->enabled()) {
            thunk->init(mem_);
            wire_thunk_glfw_cb_runner_();
            wire_thunk_sdl_thread_runner_();
        }
        if (auto* thunk = graphics_.display_thunk(); thunk && thunk->enabled())
            thunk->init(mem_);
        // The shared syscall dispatcher probes every thunk namespace.
        // An enabled but uninitialized audio thunk returns ENOSYS before
        // the dispatcher can reach an Android display symbol.
        // Wiring gives AAudio its host engine. Without dyn_linker_ the CPU
        // factory makes no guest TLS, so the pump stays off and the libaaudio
        // adapter's own Bionic thread runs data callbacks.
        if (auto* thunk = graphics_.audio_thunk(); thunk && thunk->enabled()) {
            thunk->init(mem_);
            wire_thunk_audio_runner_();
        }
        wire_thunk_android_runner_();
        if (verbose_) fprintf(stderr, "[android-runtime] linker64 base=0x%llx entry=0x%llx page_size=%llu\n",
            static_cast<unsigned long long>(interp_base_),
            static_cast<unsigned long long>(entry_),
            static_cast<unsigned long long>(Memory::PAGE_SIZE));
    } else if (!info.interp.empty()) {
        // First, try the native (in-emulator) dynamic linker. This is
        // faster and more reliable because we don't depend on the
        // guest-side ld.so working correctly under emulation.
        // Default: use the native (in-emulator) dynamic linker. It's faster
        // and more reliable than depending on the guest-side ld.so under
        // emulation. Set BIFROST_NO_NATIVE_DYNLINK=1 to fall back to the
        // guest-side ld.so (for debugging).
        bool native_dynlink = (getenv("BIFROST_NO_NATIVE_DYNLINK") == nullptr);
        if (native_dynlink) {
            dyn_linker_ = std::make_unique<DynamicLinker>(mem_);
            // ── Wire GraphicThunk into the dynamic linker ──
            // When BIFROST_THUNK_GRAPHICS=1 is set, the thunk forwards
            // guest GL/EGL/SDL2 calls to the host. We init the thunk
            // (allocates the guest trampoline page) and register a
            // resolver callback so the dynamic linker can populate the
            // global symbol table for graphic libraries that couldn't
            // be loaded from disk.
            //
            // We always wire the resolver (even if the thunk is
            // disabled) — when disabled, enumerate_symbols() returns 0
            // and the dynamic linker falls through to its existing
            // "library not found" path. This keeps the code path
            // uniform.
            if (auto* thunk = graphics_.thunk()) {
                if (thunk->enabled()) {
                    thunk->init(mem_);
                    wire_thunk_glfw_cb_runner_();
                    wire_thunk_sdl_thread_runner_();
                }
                // Capture the thunk pointer (not `this`) so the
                // callback doesn't depend on the Emulator's lifetime
                // beyond the thunk's. The thunk is owned by graphics_,
                // which is owned by the Emulator — they have the same
                // lifetime.
                GraphicThunk* thunk_ptr = thunk;
                dyn_linker_->set_thunk_resolver(
                    [thunk_ptr](const std::string& lib)
                        -> DynamicLinker::ThunkSymbolList {
                        DynamicLinker::ThunkSymbolList out;
                        if (!thunk_ptr->enabled()) return out;
                        thunk_ptr->enumerate_symbols(lib,
                            [&](const std::string& sym, uint64_t addr) {
                                out.emplace_back(sym, addr);
                            });
                        return out;
                    });
            }
            // also init AudioThunk and DisplayThunk if
            // enabled. They share the thunk syscall (0x1000) with
            // GraphicThunk; the dispatcher in HostCalls/Thunks.cpp tries each in
            // order. The dynamic linker's thunk_resolver_ is set to
            // GraphicThunk's enumerate_symbols, so audio/display symbols
            // need a different wiring path — we extend the resolver to
            // also consult the other thunks. (See set_thunk_resolver
            // extension below.)
            if (auto* athunk = graphics_.audio_thunk()) {
                if (athunk->enabled()) { athunk->init(mem_); wire_thunk_audio_runner_(); }
            }
            if (auto* dthunk = graphics_.display_thunk()) {
                if (dthunk->enabled()) {
                    dthunk->init(mem_);
                    wire_thunk_wl_cb_runner_();
                    wire_thunk_wl_fd_publisher_();
                    dthunk->set_wl_fd_resolver([this](int gfd) -> int {
                        auto n = fds_.get(gfd);
                        if (!n) return -1;
                        return n->host_fd();
                    });
                }
            }
            // Extend the thunk resolver to consult all three thunks.
            // Each thunk has its own per-library symbol enumeration; we
            // merge their results so the dynamic linker sees the union.
            {
                GraphicThunk* gthunk = graphics_.thunk();
                AudioThunk*   athunk = graphics_.audio_thunk();
                DisplayThunk* dthunk = graphics_.display_thunk();
                dyn_linker_->set_thunk_resolver(
                    [gthunk, athunk, dthunk](const std::string& lib)
                        -> DynamicLinker::ThunkSymbolList {
                        DynamicLinker::ThunkSymbolList out;
                        auto add_all = [&](auto* t) {
                            if (!t || !t->enabled()) return;
                            t->enumerate_symbols(lib,
                                [&](const std::string& sym, uint64_t addr) {
                                    out.emplace_back(sym, addr);
                                });
                        };
                        add_all(gthunk);
                        add_all(athunk);
                        add_all(dthunk);
                        return out;
                });
                // vkQuake loads every Vulkan entry point through
                // SDL_Vulkan_GetVkGetInstanceProcAddr — GraphicThunk needs
                // the DisplayThunk's guest trampoline registry to answer it.
                if (gthunk && dthunk && dthunk->enabled()) {
                    gthunk->set_vk_proc_lookup([dthunk](const char* sym) {
                        return dthunk->resolve("libvulkan.so.1", sym);
                    });
                }
            }
            wire_thunk_android_runner_();
            // Register the ifunc resolver callback. BUGFIX: the old
            // IRELATIVE handler just stored the resolver ADDRESS instead
            // of CALLING it. Now we run the resolver in a scratch CPU
            // state (borrowing the caller's parked CPU, which hasn't
            // been initialized yet during startup link() — its real init
            // happens later). The resolver is a small guest function that
            // returns a function pointer in X0; we run it via step
            // until it RETs to a sentinel address, then capture X0.
            dyn_linker_->set_ifunc_resolver([this](CPU& cpu, uint64_t resolver_addr) -> uint64_t {
                if (resolver_addr == 0) return 0;
                // Borrow `cpu` as scratch — this is the CALLING thread's
                // parked CPU (cpu during startup link(), which is
                // still-uninitialized and idle). Save its architectural
                // state (regs, sp, pc, pstate, FP regs, TLS, sigmask) so we
                // don't disturb the caller's syscall context. We can't copy
                // the whole CPU struct because it has mutex + atomic members
                // that are non-copyable. The pending-queue state is
                // irrelevant — ifunc resolution runs at load time before
                // any signals can be pending.
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
                static_assert(sizeof(saved.regs) == sizeof(cpu.regs), "");
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
                // Guarded scratch stack for the resolver ([GUARD][usable]
                // [GUARD], PROT_NONE guards): overflow faults loudly
                // instead of silently corrupting adjacent chunks. Reused
                // per host thread (keyed by owner so a second Emulator on
                // the same thread doesn't reuse an unmapped window);
                // allocating fresh here leaked 64 KiB + guards per call.
                static thread_local uint64_t scratch_stack = 0;
                static thread_local const Emulator* scratch_owner = nullptr;
                if (scratch_stack == 0 || scratch_owner != this) {
                    scratch_stack = mem_.mmap_alloc_callback_stack(64 * 1024);
                    scratch_owner = this;
                    if (scratch_stack == 0) { restore(); return 0; }
                }
                uint64_t stack_top = scratch_stack + 64 * 1024;
                // Sentinel return address — when PC == this, the resolver
                // has RET'd. Use 0x1000 (in the zero page, unmapped for
                // execution but a recognizable sentinel).
                constexpr uint64_t SENTINEL_LR = 0x1000;
                cpu.pc = resolver_addr;
                cpu.sp = stack_top;
                cpu.regs[30] = SENTINEL_LR;  // LR
                cpu.running = true;
                cpu.pstate = 0;
                // Run the resolver. Cap at 1M instructions to avoid
                // infinite loops in buggy resolvers.
                constexpr uint64_t IRESOLVER_LIMIT = 1'000'000;
                uint64_t steps = 0;
                try {
                    while (cpu.running && cpu.pc != SENTINEL_LR
                           && steps < IRESOLVER_LIMIT) {
                        step(cpu);
                        steps++;
                    }
                } catch (const std::exception& e) {
                    if (getenv("BIFROST_IFUNC_TRACE")) {
                        fprintf(stderr, "[%s] ifunc resolver at 0x%llx threw: %s\n",
                                CODENAME,
                                static_cast<unsigned long long>(resolver_addr),
                                e.what());
                    }
                    restore();
                    return 0;
                }
                uint64_t result = cpu.regs[0];
                if (steps >= IRESOLVER_LIMIT) {
                    if (getenv("BIFROST_IFUNC_TRACE")) {
                        fprintf(stderr, "[%s] ifunc resolver at 0x%llx ran >%llu "
                                "instructions; aborting (likely infinite loop)\n",
                                CODENAME,
                                static_cast<unsigned long long>(resolver_addr),
                                static_cast<unsigned long long>(IRESOLVER_LIMIT));
                    }
                    result = 0;
                }
                if (getenv("BIFROST_IFUNC_TRACE")) {
                    fprintf(stderr, "[%s] ifunc resolver at 0x%llx returned 0x%llx "
                            "(steps=%llu)\n",
                            CODENAME,
                            static_cast<unsigned long long>(resolver_addr),
                            static_cast<unsigned long long>(result),
                            static_cast<unsigned long long>(steps));
                }
                // Restore cpu to its pre-resolver state.
                restore();
                return result;
            });
            // dynamic linker can invoke DT_INIT_ARRAY entries (C++ static
            // constructors, glibc hooks, etc.). Uses the same borrow-CPU
            // pattern as the ifunc resolver, but doesn't capture X0 —
            // just runs the function until it RETs.
            dyn_linker_->set_init_runner([this](CPU& cpu, uint64_t fn_addr) {
                if (fn_addr == 0) return;
                // Save architectural state (same pattern as ifunc resolver).
                // `cpu` is the caller's parked CPU (cpu at startup).
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
                // before running init functions. Constructors and
                // __libc_early_init access TLS variables (e.g., ctype
                // tables, locale pointers) via TPIDR_EL0. Without this,
                // TLS variables are at wrong addresses and initialization
                // fails silently.
                if (dyn_linker_ && dyn_linker_->static_tls_size() > 0 &&
                    cpu.tpidr_el0 == 0) {
                    // Borrowed CPU has no TLS pointer yet (startup path —
                    // main_cpu_ is still default-initialized). Set it to the
                    // static TLS block. A parked guest thread (runtime dl*)
                    // already has its own per-thread TPIDR_EL0; leave it so
                    // the guest constructors see ITS TLS, not the main
                    // thread's.
                    // Variant-I: TP = base + libc_size (TCB header start).
                    cpu.tpidr_el0 = dyn_linker_->thread_pointer();
                    cpu.tpidrro_el0 = cpu.tpidr_el0;
                }
                // Guarded scratch stack for the constructor (PROT_NONE
                // guard pages both sides). Reused per host thread (see the
                // ifunc resolver above).
                static thread_local uint64_t scratch_stack = 0;
                static thread_local const Emulator* scratch_owner = nullptr;
                if (scratch_stack == 0 || scratch_owner != this) {
                    scratch_stack = mem_.mmap_alloc_callback_stack(64 * 1024);
                    scratch_owner = this;
                    if (scratch_stack == 0) { restore(); return; }
                }
                uint64_t stack_top = scratch_stack + 64 * 1024;
                constexpr uint64_t SENTINEL_LR = 0x1000;
                cpu.pc = fn_addr;
                cpu.sp = stack_top;
                cpu.regs[30] = SENTINEL_LR;
                cpu.running = true;
                cpu.pstate = 0;
                // Constructors may call libc functions (e.g., printf for
                // debug logging), which may themselves use atomics / TLS.
                // Cap at 10M instructions to avoid infinite loops in
                // buggy constructors.
                constexpr uint64_t INIT_LIMIT = 10'000'000;
                uint64_t steps = 0;
                try {
                    while (cpu.running && cpu.pc != SENTINEL_LR
                           && steps < INIT_LIMIT) {
                        step(cpu);
                        steps++;
                    }
                } catch (const std::exception& e) {
                    if (dbg().dynlink_trace) {
                        fprintf(stderr, "[%s] init_array @ 0x%llx threw: %s\n",
                                CODENAME,
                                static_cast<unsigned long long>(fn_addr),
                                e.what());
                    }
                }
                if (steps >= INIT_LIMIT && dbg().dynlink_trace) {
                    fprintf(stderr, "[%s] init_array @ 0x%llx ran >%llu "
                            "instructions; aborting (likely infinite loop)\n",
                            CODENAME,
                            static_cast<unsigned long long>(fn_addr),
                            static_cast<unsigned long long>(INIT_LIMIT));
                }
                restore();
            });
            // Argument-passing guest-call callback. Same borrow-CPU
            // pattern as init_runner_, but sets x0/x1/x2 before running
            // and returns x0. Used by dl_iterate_phdr (callback+data)
            // and dlopen init arrays (argc/argv/env). Step limit is
            // 50M (higher than init_runner_'s 10M) because dl_iterate_phdr
            // callbacks may do significant work (e.g., backtrace walking).
            dyn_linker_->set_guest_call_args([this](CPU& cpu,
                                                      uint64_t fn_addr,
                                                      uint64_t arg0,
                                                      uint64_t arg1,
                                                      uint64_t arg2) -> uint64_t {
                if (fn_addr == 0) return 0;
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
                // Set up TLS pointer (same as init_runner_). Only when the
                // borrowed CPU has none (startup). A parked guest thread
                // already has its own per-thread TPIDR_EL0.
                if (dyn_linker_ && dyn_linker_->static_tls_size() > 0 &&
                    cpu.tpidr_el0 == 0) {
                    cpu.tpidr_el0 = dyn_linker_->thread_pointer();
                    cpu.tpidrro_el0 = cpu.tpidr_el0;
                }
                // Reusable scratch stack (thread-local, one per guest
                // thread). The old code mmap'd a fresh 64 KiB stack on
                // EVERY call and never freed it: dl_iterate_phdr-heavy
                // guests (dlopen_mt: 8 threads × callbacks × iterations)
                // marched the heap into OOM, and a 0 return put the
                // guest stack at 0x10000 (wild writes). Mirrors the
                // thread-local reuse in the static-ELF guest_call_args_
                // below.
                static thread_local uint64_t scratch_stack = 0;
                static thread_local const Emulator* scratch_owner = nullptr;
                if (scratch_stack == 0 || scratch_owner != this) {
                    scratch_stack = mem_.mmap_alloc_callback_stack(64 * 1024);
                    scratch_owner = this;
                    if (scratch_stack == 0) return 0;
                }
                uint64_t stack_top = scratch_stack + 64 * 1024;
                constexpr uint64_t SENTINEL_LR = 0x1000;
                cpu.pc = fn_addr;
                cpu.sp = stack_top;
                cpu.regs[0] = arg0;
                cpu.regs[1] = arg1;
                cpu.regs[2] = arg2;
                cpu.regs[30] = SENTINEL_LR;
                cpu.running = true;
                cpu.pstate = 0;
                constexpr uint64_t CALL_LIMIT = 50'000'000;
                uint64_t steps = 0;
                uint64_t result = 0;
                try {
                    while (cpu.running && cpu.pc != SENTINEL_LR
                           && steps < CALL_LIMIT) {
                        step(cpu);
                        steps++;
                    }
                    result = cpu.regs[0];
                } catch (const std::exception& e) {
                    if (dbg().dynlink_trace) {
                        fprintf(stderr, "[%s] guest_call @ 0x%llx threw: %s\n",
                                CODENAME,
                                static_cast<unsigned long long>(fn_addr),
                                e.what());
                    }
                    result = 0;
                }
                if (steps >= CALL_LIMIT && dbg().dynlink_trace) {
                    fprintf(stderr, "[%s] guest_call @ 0x%llx ran >%llu "
                            "instructions; aborting\n",
                            CODENAME,
                            static_cast<unsigned long long>(fn_addr),
                            static_cast<unsigned long long>(CALL_LIMIT));
                    result = 0;
                }
                restore();
                return result;
            });
            if (!dyn_linker_->link(main_cpu_, data, info.base_addr, path, info.interp)) {
                fprintf(stderr, "[%s] native dynamic linking failed: %s; "
                        "falling back to guest ld.so\n",
                        CODENAME, dyn_linker_->error().c_str());
                dyn_linker_.reset();
            } else {
                libc_single_threaded_addr_ =
                    dyn_linker_->libc_single_threaded_addr();
            }
            if (dyn_linker_ && dyn_linker_->static_tls_size() > 0) {
                // Set up TPIDR_EL0 using variant-I TLS layout (glibc AArch64):
                //   TP = static_tls_base + lib_size
                // The TCB header (tcbhead_t) is at [TP, TP + tcb_size).
                // Main exe TLS is at [TP + tcb_size, ...) (positive offset).
                // Lib TLS is at [TP - lib_size, TP) (negative offset).
                //
                // END of the block, variant-II). This broke local-exec TLS
                // access for the main exe — the hardcoded positive TPREL
                // offset landed in the wrong place. With variant-I, TP points
                // to the TCB header start (= base + lib_size).
                uint64_t tp = dyn_linker_->thread_pointer();
                main_cpu_.tpidr_el0 = tp;
                main_cpu_.tpidrro_el0 = tp;
                dyn_linker_->register_thread_tls_block(tp);
                if (verbose_) {
                    fprintf(stderr, "[%s] native dynlink: static TLS block "
                            "at 0x%llx (size %llu), TP=0x%llx (variant-I)\n",
                            CODENAME,
                            static_cast<unsigned long long>(dyn_linker_->static_tls_base()),
                            static_cast<unsigned long long>(dyn_linker_->static_tls_size()),
                            static_cast<unsigned long long>(tp));
                }
            }
            // Even with native dynlink, we still load the interpreter
            // (ld.so) because some programs call ld.so's _dl_*
            // functions directly. The interpreter's symbols are
            // available via the native linker's symbol table.
        }
        // Try to open the interpreter. Check common host paths for
        // aarch64 dynamic linkers (musl and glibc multiarch).
        std::vector<std::string> interp_paths = {
            info.interp,                                    // exact path from PT_INTERP
            "/usr/aarch64-linux-gnu" + info.interp,         // Debian/Ubuntu multiarch
            "/usr/lib/aarch64-linux-gnu" + info.interp,     // newer Debian multiarch
        };
        FILE* ifile = nullptr;
        std::string found_path;
        for (auto& ip : interp_paths) {
            ifile = fopen(ip.c_str(), "rb");
            if (ifile) { found_path = ip; break; }
        }
        if (ifile) {
            fseek(ifile, 0, SEEK_END);
            long isz = ftell(ifile);
            fseek(ifile, 0, SEEK_SET);
            if (isz <= 0) { fclose(ifile); throw EmuError("empty or invalid interpreter"); }
            std::vector<uint8_t> idata(isz);
            if (fread(idata.data(), 1, isz, ifile) != static_cast<size_t>(isz)) {
                fclose(ifile);
                throw EmuError("short read on interpreter");
            }
            fclose(ifile);
            // Load the interpreter at a high address to avoid conflicts.
            // 0x4000000000 (256 GiB) is above the low region and the heap.
            interp_base = 0x4000000000ULL;
            interp_base_ = interp_base;  // Save for AT_BASE in auxv.
            Memory::FileIdentity interp_file;
            interp_file.host_path = true;
            if (char* resolved = ::realpath(found_path.c_str(), nullptr)) {
                interp_file.path = resolved;
                free(resolved);
            } else {
                interp_file.path = found_path;
            }
            struct stat interp_stat{};
            if (::stat(interp_file.path.c_str(), &interp_stat) == 0) {
                interp_file.dev = interp_stat.st_dev;
                interp_file.inode = interp_stat.st_ino;
            }
            if (idata.size() >= 64 && idata[0] == 0x7f && idata[1] == 'E') {
                uint64_t i_entry, i_phoff;
                uint16_t i_phnum, i_phentsize;
                memcpy(&i_entry, idata.data() + 24, 8);
                memcpy(&i_phoff, idata.data() + 32, 8);
                memcpy(&i_phentsize, idata.data() + 54, 2);
                memcpy(&i_phnum, idata.data() + 56, 2);
                for (int i = 0; i < i_phnum; i++) {
                    const uint8_t* pp = idata.data() + i_phoff + i * i_phentsize;
                    uint32_t p_type;
                    uint64_t p_offset, p_vaddr, p_filesz, p_memsz;
                    memcpy(&p_type, pp + 0, 4);
                    memcpy(&p_offset, pp + 8, 8);
                    memcpy(&p_vaddr, pp + 16, 8);
                    memcpy(&p_filesz, pp + 32, 8);
                    memcpy(&p_memsz, pp + 40, 8);
                    if (p_type != 1) continue;  // PT_LOAD
                    uint64_t addr = interp_base + p_vaddr;
                    mem_.map_range(addr, p_memsz);
                    if (p_filesz > 0) {
                        mem_.write(addr, idata.data() + p_offset, p_filesz);
                        mem_.name_image(addr, p_filesz, p_offset, interp_file);
                    }
                }
                // The entry point is the interpreter's entry (relative to interp_base).
                // Skip this if we used native dynlink — we run the main
                // binary's entry directly.
                if (!native_dynlink || !dyn_linker_) {
                    entry_ = interp_base + i_entry;
                }
            }
        }
    }
    // Static binaries have no PT_INTERP, so the block above never runs —
    // but GraphicThunk / runtime dlopen (syscalls 0x1002/0x1003) still
    // need a DynamicLinker + thunk trampolines. Create a minimal one.
    if (!android_runtime_ && !dyn_linker_) {
        dyn_linker_ = std::make_unique<DynamicLinker>(mem_);
        if (auto* thunk = graphics_.thunk()) {
            if (thunk->enabled()) {
                thunk->init(mem_);
                wire_thunk_glfw_cb_runner_();
                wire_thunk_sdl_thread_runner_();
            }
        }
        if (auto* athunk = graphics_.audio_thunk()) {
            if (athunk->enabled()) { athunk->init(mem_); wire_thunk_audio_runner_(); }
        }
        if (auto* dthunk = graphics_.display_thunk()) {
            if (dthunk->enabled()) {
                dthunk->init(mem_);
                wire_thunk_wl_cb_runner_();
                wire_thunk_wl_fd_publisher_();
                dthunk->set_wl_fd_resolver([this](int gfd) -> int {
                    auto n = fds_.get(gfd);
                    if (!n) return -1;
                    return n->host_fd();
                });
            }
        }
        GraphicThunk* gthunk = graphics_.thunk();
        AudioThunk*   athunk = graphics_.audio_thunk();
        DisplayThunk* dthunk = graphics_.display_thunk();
        dyn_linker_->set_thunk_resolver(
            [gthunk, athunk, dthunk](const std::string& lib)
                -> DynamicLinker::ThunkSymbolList {
                DynamicLinker::ThunkSymbolList out;
                auto add_all = [&](auto* t) {
                    if (!t || !t->enabled()) return;
                    t->enumerate_symbols(lib,
                        [&](const std::string& sym, uint64_t addr) {
                            out.emplace_back(sym, addr);
                        });
                };
                add_all(gthunk);
                add_all(athunk);
                add_all(dthunk);
                return out;
            });
        wire_thunk_android_runner_();
    }
    // brk starts just above the loaded image, page-aligned up
    brk_ = (info.end_addr + 0xFFF) & ~0xFFFULL;
    brk_start_ = brk_;
    // Set up the initial stack image (low region, below 1008 MiB); its
    // top carries per-process ASLR jitter.
    const uint64_t STACK_TOP = mem_.stack_top();
    const uint64_t STACK_SIZE = Memory::STACK_SIZE;
    uint64_t stack_base = STACK_TOP - STACK_SIZE;
    mem_.map_range(stack_base, STACK_SIZE + 4096);  // +1 page guard at top
    // load the embedded vDSO before building the stack so
    // AT_SYSINFO_EHDR can point to it. The vDSO provides
    // gettimeofday/clock_gettime/clock_getres/rt_sigreturn stubs.
    load_vdso();
    main_cpu_.sp = build_initial_stack(STACK_TOP, argv, info);
    // glibc's ld.so normally sets __environ during _dl_start_user; the
    // native dynlink path skips ld.so, so point __environ at the envp
    // array on the initial stack before the program's _start runs.
    if (dyn_linker_) {
        dyn_linker_->set_guest_environ(guest_envp_addr_);
        dyn_linker_->set_libc_stack_end(main_cpu_.sp);
    }
    // Pre-allocate a TLS scratch area and set TPIDR_EL0 to point into
    // its center. Many libc startup routines read TPIDR_EL0 before
    // __libc_setup_tls has set the real TCB. Pointing it to valid
    // zeroed memory prevents unmapped-read crashes.
    //
    // binaries, the dynamic linker already set TPIDR_EL0 to the static
    // TLS block (line ~507 above). Overwriting it here destroys libc's
    // TLS — libc reads errno, stdin/stdout/stderr FILE pointers, locale
    // pointers, etc. via TPIDR_EL0-relative loads. With the scratch
    // area, all those reads return 0, causing stdio functions to crash
    // (NULL vtable pointer → blr x16 with x16=0 → pc=0 → decode error).
    // The dynamic linker's TPIDR_EL0 is correct and must be preserved.
    //
    // Static binaries may still have a DynamicLinker for thunk/dlopen
    // support without a static TLS block — only skip scratch when the
    // linker actually installed a TLS TP.
    if (!android_runtime_ && (!dyn_linker_ || dyn_linker_->static_tls_size() == 0)) {
        if (main_cpu_.tpidr_el0 == 0) {
            const uint64_t TLS_SCRATCH_SIZE = 65536;  // 64 KiB
            uint64_t tls_scratch = mem_.mmap_alloc(TLS_SCRATCH_SIZE);
            main_cpu_.tpidr_el0 = tls_scratch + TLS_SCRATCH_SIZE / 2;
            main_cpu_.tpidrro_el0 = main_cpu_.tpidr_el0;
        }
    }
    // Map the zero page so NULL dereferences return 0 instead of
    // crashing. libc code often has NULL checks that only work if
    // the load itself doesn't fault. Not for Android (see above).
    if (!android_runtime_) mem_.map_range(0, 4096);
    main_cpu_.pc = entry_;
    main_cpu_.running = true;
    main_cpu_.tid = vfs_.self_pid();  // A process leader's TID equals its PID.
    next_tid_ = 2;
    main_cpu_.set_tid_address_ptr = 0;
}
}  // namespace arm64emu
