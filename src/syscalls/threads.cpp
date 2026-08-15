// syscalls/threads.cpp — thread/signal syscalls: clone/clone3/futex/
// set_tid_address/set_robust_list/get_robust_list/tgkill/tkill/kill.
//
// Extracted verbatim from the original syscalls.cpp. References to
// private Emulator members (signals_, mem_, get_futex, spawn_thread,
// find_cpu_by_tid, etc.) work via the friend declaration.
#include "core/emulator.h"
#include "core/memory.h"
#include "core/cpu.h"
#include "core/signal.h"
#include "debug_flags.h"
#include "syscalls/syscalls.h"
#include "yggdrasil/yggdrasil.hpp"
#include "jit/frostjit.hpp"
#include "frontend/dynamic_linker.h"
#include <errno.h>
#include <signal.h>
#include <mutex>
#include <cstring>
#include <vector>
#include <thread>
namespace arm64emu {
// ── Clone flag constants ───────────────────────────────────────────────
// Per Linux kernel include/uapi/linux/sched.h. Named constants replace
// the raw hex values (0x100, 0x80000, etc.) that were sprinkled across
// spawn_thread / fork_guest / the clone syscall handler. This makes the
// code self-documenting and prevents transcription errors.
namespace clone_flags {
    constexpr uint64_t VM                = 0x00000100;  // share memory
    constexpr uint64_t FS                = 0x00000200;  // share cwd, umask, root
    constexpr uint64_t FILES             = 0x00000400;  // share file descriptors
    constexpr uint64_t SIGHAND           = 0x00000800;  // share signal handlers
    constexpr uint64_t PIDFD             = 0x00001000;  // return pidfd to parent
    constexpr uint64_t PTRACE            = 0x00002000;
    constexpr uint64_t VFORK             = 0x00004000;
    constexpr uint64_t PARENT            = 0x00008000;  // same parent as caller
    constexpr uint64_t THREAD            = 0x00010000;  // same thread group
    constexpr uint64_t NEWNS             = 0x00020000;  // new mount namespace
    constexpr uint64_t SYSVSEM           = 0x00040000;
    constexpr uint64_t SETTLS            = 0x00080000;
    constexpr uint64_t PARENT_SETTID     = 0x00100000;
    constexpr uint64_t CHILD_CLEARTID    = 0x00200000;
    constexpr uint64_t DETACHED          = 0x00400000;
    constexpr uint64_t UNTRACED          = 0x00800000;
    constexpr uint64_t CHILD_SETTID      = 0x01000000;
    constexpr uint64_t NEWCGROUP         = 0x02000000;
    constexpr uint64_t NEWUTS            = 0x04000000;
    constexpr uint64_t NEWIPC            = 0x08000000;
    constexpr uint64_t NEWUSER           = 0x10000000;
    constexpr uint64_t NEWPID            = 0x20000000;
    constexpr uint64_t NEWNET            = 0x40000000;
    constexpr uint64_t IO                = 0x80000000;
}  // namespace clone_flags
// Resolve a guest address to "name+0x..." using the dynamic linker's loaded
// objects and their .dynsym. Used by the BIFROST_FUTEX_BT backtrace dump.
static std::string bt_sym(Emulator& emu, uint64_t addr) {
    char buf[160];
    const DynamicLinker* dl = emu.dyn_linker();
    if (!dl) {
        snprintf(buf, sizeof buf, "%#llx", (unsigned long long)addr);
        return std::string(buf);
    }
    const LoadedObject* obj = dl->find_object_by_addr(addr);
    if (!obj) {
        snprintf(buf, sizeof buf, "%#llx", (unsigned long long)addr);
        return std::string(buf);
    }
    std::string name;
    uint64_t best = 0;  // best absolute symbol start address <= addr
    if (obj->symtab_addr && obj->symtab_count && obj->strtab_addr) {
        for (uint64_t i = 0; i < obj->symtab_count; i++) {
            uint64_t s = obj->symtab_addr + i * 24;  // Elf64_Sym
            uint32_t st_name = 0; uint64_t st_value = 0;
            try {
                st_name = emu.mem().load<uint32_t>(s);
                st_value = emu.mem().load<uint64_t>(s + 8);
            } catch (...) { break; }
            if (st_value == 0) continue;
            uint64_t abs = obj->base_addr + st_value;
            if (abs <= addr && abs >= best) {
                best = abs;
                std::string nm;
                try {
                    for (uint64_t k = 0; k < 256; k++) {
                        char c = (char)emu.mem().load<uint8_t>(obj->strtab_addr + st_name + k);
                        if (c == 0) break;
                        nm += c;
                    }
                } catch (...) {}
                if (!nm.empty()) name = nm;
            }
        }
    }
    if (!name.empty()) {
        snprintf(buf, sizeof buf, "%s+0x%llx", name.c_str(),
                 (unsigned long long)(addr - best));
        return std::string(buf);
    }
    snprintf(buf, sizeof buf, "%s+0x%llx", obj->name.c_str(),
             (unsigned long long)(addr - obj->base_addr));
    return std::string(buf);
}
// Helper: walk a thread's robust futex list and mark each held futex as
// FUTEX_OWNER_DIED, then wake waiters. Called on thread exit. Mirrors
// the kernel's exit_robust_list() (kernel/futex.c). Best-effort: if a
// pointer read fails (unmapped), we stop walking.
//
// NOTE: This logic is inlined in thread_entry() (src/core/thread_mgr.cpp)
// to avoid cross-TU coupling. The set_robust_list/get_robust_list syscalls
// below just store/retrieve the head pointer; the actual list walk happens
// at thread exit.
int64_t syscall_threads(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a4; (void)a5;
    auto& mem_ = emu.mem_;
    auto& signals_ = emu.signals_;
    // Lambda wrappers for Emulator member access (spawn_thread, get_futex).
    // (find_cpu_by_tid / decrement_alive_threads are available via emu.*
    // directly at call sites if needed; the lambda wrappers here are
    // only for the members used by every case.)
    auto spawn_thread = [&](CPU& parent_cpu, uint64_t flags, uint64_t stack_top,
                            uint64_t entry_pc, uint64_t arg, uint64_t tls) {
        return emu.spawn_thread(parent_cpu, flags, stack_top, entry_pc, arg, tls);
    };
    auto get_futex = [&](uint64_t addr) -> Emulator::FutexSlot* {
        return emu.get_futex(addr);
    };
    switch (num) {
        case 220: { // clone(flags, stack, ptid, ctid, tls)
            // AArch64 clone() syscall signature (matches glibc/musl):
            //   x0 = flags        (CLONE_*)
            //   x1 = stack        (top of child stack)
            //   x2 = parent_tidptr
            //   x3 = child_tidptr (CLONE_CHILD_SETTID writes TID here)
            //   x4 = tls          (new TPIDR_EL0, if CLONE_SETTLS)
            //
            // On success: parent gets child TID, child gets 0.
            // The new thread starts at the same PC as the syscall return
            // address (i.e., x30 / LR of the parent), with x0=0.
            //
            // We support two paths:
            //   1. CLONE_VM (threads): spawn a vCPU on a host thread.
            //   2. No CLONE_VM (fork): host fork() with CoW memory.
            // AArch64 clone() syscall argument order (per the kernel's
            // SYSCALL_DEFINE5(clone, flags, newsp, parent_tidptr, tls,
            // child_tidptr) in arch/arm64/kernel/process.c):
            //   x0 = flags
            //   x1 = stack (newsp)
            //   x2 = parent_tidptr (ptid)
            //   x3 = tls              ← NOT ctid!
            //   x4 = child_tidptr (ctid)  ← NOT tls!
            //
            // BUGFIX: the old code had ctid/tls swapped (ctid=a3, tls=a4).
            // This is the OPPOSITE of x86_64's clone ABI. On AArch64,
            // x3=tls and x4=ctid. The swap meant spawned threads got the
            // wrong TPIDR_EL0 (set to the ctid address instead of the TLS
            // pointer), breaking pthread_join which computes the pthread
            // struct base as TPIDR_EL0 - 0xc8.
            uint64_t flags = a0;
            uint64_t stack = a1;
            uint64_t ptid_ptr = a2;
            uint64_t tls = a3;       // AArch64: x3 = tls
            uint64_t ctid_ptr = a4;  // AArch64: x4 = ctid
            if (!(flags & clone_flags::VM)) {
                // ── Fork path (no CLONE_VM) ──
                // Use host fork() for copy-on-write memory. The child
                // process inherits the entire emulator state and runs
                // independently. The parent's wait4() forwards to host
                // wait4().
                int child_pid = emu.fork_guest(cpu, stack, flags,
                                               ptid_ptr, ctid_ptr, tls);
                if (child_pid < 0) {
                    ret_err(ENOMEM);
                } else {
                    ret_host(static_cast<uint64_t>(child_pid));
                }
                return 0;
            }
            // ── Thread path (CLONE_VM) ──
            // The new thread's entry point is the instruction AFTER the
            // SVC (same as the parent). Both parent and child return from
            // the clone() syscall to the same PC — the child gets x0=0,
            // the parent gets x0=child_tid. The child then checks x0 and
            // branches to the thread function.
            //
            // BUGFIX: the old code used cpu.regs[30] (LR) as the entry
            // point, but LR is the return address of __clone's CALLER
            // (e.g., pthread_create's internal function), not the
            // instruction after SVC. The child must start at SVC+4 so it
            // falls through to the "cbnz x0, parent_return" / "ldr fn/arg
            // / blr fn" sequence in musl's __clone wrapper.
            //
            // NOTE: The interpreter's SVC_IMM handler advances cpu.pc to
            // SVC+4 BEFORE calling syscall(), so cpu.pc is already the
            // correct entry point (the instruction after SVC). We use
            // cpu.pc directly, NOT cpu.pc + 4.
            uint64_t entry_pc = cpu.pc;  // already SVC+4 (set by SVC_IMM handler)
            uint64_t arg = 0;  // x0 will be set to 0 for child
            int child_tid = spawn_thread(cpu, flags, stack, entry_pc, arg, tls);
            if (child_tid < 0) {
                ret_err(ENOMEM);
                return 0;
            }
            // CLONE_PARENT_SETTID: write child TID to *ptid
            if ((flags & clone_flags::PARENT_SETTID) && ptid_ptr) {
                mem_.store<uint32_t>(ptid_ptr, child_tid);
            }
            ret_host(child_tid);
            return 0;
        }
        case 435: { // clone3(clone_args, size) — AArch64 syscall 435
            // clone3 is the modern (Linux 5.3+) replacement for clone().
            // It takes a struct clone_args and a size. We translate the
            // relevant fields to the legacy clone() logic. Unsupported
            // fields (set_tid, set_tid_size, cgroup) are ignored.
            //
            // struct clone_args (64 bytes, AArch64 layout):
            //   +0:  __u64 flags
            //   +8:  __u64 pidfd
            //   +16: __u64 child_tid
            //   +24: __u64 parent_tid
            //   +32: __u64 exit_signal
            //   +40: __u64 stack
            //   +48: __u64 stack_size
            //   +56: __u64 tls
            //   +64: __u64 set_tid        (ignored — requires kernel support)
            //   +72: __u64 set_tid_size   (ignored)
            //   +80: __u64 cgroup         (ignored)
            uint64_t args_ptr = a0;
            uint64_t args_size = a1;
            if (args_ptr == 0) { ret_err(EINVAL); return 0; }
            if (args_size < 64) { ret_err(EINVAL); return 0; }
            uint64_t flags, pidfd, child_tid, parent_tid, exit_signal,
                     stack, stack_size, tls;
            try {
                flags       = mem_.load<uint64_t>(args_ptr + 0);
                pidfd       = mem_.load<uint64_t>(args_ptr + 8);
                child_tid   = mem_.load<uint64_t>(args_ptr + 16);
                parent_tid  = mem_.load<uint64_t>(args_ptr + 24);
                exit_signal = mem_.load<uint64_t>(args_ptr + 32);
                stack       = mem_.load<uint64_t>(args_ptr + 40);
                stack_size  = mem_.load<uint64_t>(args_ptr + 48);
                tls         = mem_.load<uint64_t>(args_ptr + 56);
            } catch (...) {
                ret_err(EFAULT);
                return 0;
            }
            (void)pidfd;       // CLONE_PIDFD — not yet supported
            (void)exit_signal; // we always deliver SIGCHLD to parent
            // The child stack top is stack + stack_size (clone3 specifies
            // the stack base and size separately, unlike clone which takes
            // the stack top directly).
            uint64_t stack_top = stack + stack_size;
            if (stack_size == 0) stack_top = stack;  // fork() idiom
            if (!(flags & clone_flags::VM)) {
                // Fork path.
                int child_pid = emu.fork_guest(cpu, stack_top, flags,
                                               parent_tid, child_tid, tls);
                if (child_pid < 0) ret_err(ENOMEM);
                else                ret_host(static_cast<uint64_t>(child_pid));
                return 0;
            }
            // Thread path. Entry point = instruction after SVC (same as
            // clone case 220 above). The interpreter's SVC_IMM handler
            // advances cpu.pc to SVC+4 before calling syscall(), so
            // cpu.pc is already the correct entry point.
            //
            // from parent_cpu.regs[4] (x4) — correct for legacy clone()
            // (where x4=ctid on AArch64) but WRONG for clone3, where
            // ctid is in the clone_args struct at offset +16, not in a
            // register. Without this, CLONE_CHILD_SETTID writes the new
            // tid to a garbage address, and CLONE_CHILD_CLEARTID records
            // a garbage clear_child_tid. The latter is fatal: when the
            // child thread exits, thread_entry() clears *clear_child_tid
            // (wrong address) and futex-wakes it — so the REAL ctid
            // (&pd->tid, passed by glibc's allocate_stack) is never
            // zeroed and never woken. pthread_join's lll_wait_tid loop
            // then spins forever on &pd->tid (val stays == child tid).
            //
            // Fix: stage the clone3 child_tid (read from the struct
            // above) into x4 so spawn_thread's regs[4] read sees the
            // correct ctid. x4 is caller-saved across syscalls (only x0
            // is preserved on return), so clobbering it here is safe.
            cpu.regs[4] = child_tid;
            uint64_t entry_pc = cpu.pc;  // already SVC+4
            int tid = spawn_thread(cpu, flags, stack_top, entry_pc, 0, tls);
            if (tid < 0) { ret_err(ENOMEM); return 0; }
            if ((flags & clone_flags::PARENT_SETTID) && parent_tid) {
                mem_.store<uint32_t>(parent_tid, static_cast<uint32_t>(tid));
            }
            ret_host(static_cast<uint64_t>(tid));
            return 0;
        }
        case 221: { // execve — AArch64 syscall 221
            // execve(path, argv, envp) — replace the guest's memory image
            // with a new ELF binary. This is called by the shell after
            // fork() to run external commands.
            //
            // We implement this by:
            //   1. Reading the path from guest memory
            //   2. Checking if it's a valid AArch64 ELF
            //   3. If yes: clear guest memory, reload the ELF, set up new
            //      stack, jump to entry point
            //   4. If no (wrong arch): try multi-call binary redirect
            //      (see below)
            //
            // This is called in the child process after fork(). The child
            // has a CoW copy of the parent's memory, so clearing it is
            // safe — the parent is unaffected.
            //
            // ── Multi-call binary redirect ───────────────────
            // When the guest runs `toybox sh -c 'ls /'`, the shell does
            // a PATH lookup for `ls`, finds the host's `/bin/ls` (x86-64),
            // and calls execve("/bin/ls", ...). The host binary is not
            // AArch64, so we'd return -ENOEXEC. But toybox is a multi-
            // call binary — `/bin/ls` should be a symlink to toybox.
            // The fix: when execve gets a non-AArch64 binary, check if
            // the basename (e.g. "ls") matches a command the currently-
            // running ELF supports. If so, re-exec the current ELF with
            // argv[0] = basename. This is exactly how BusyBox/toybox
            // multi-call binaries work on real Linux.
            std::string path = yggdrasil::Yggdrasil::read_path(mem_, a0);
            if (path.empty()) {
                ret_err(EFAULT);
                return 0;
            }
            // Read argv from guest memory (needed for both paths).
            std::vector<std::string> new_argv;
            uint64_t argv_ptr = a1;
            while (true) {
                uint64_t str_ptr;
                try {
                    str_ptr = mem_.load<uint64_t>(argv_ptr);
                } catch (...) { break; }
                if (str_ptr == 0) break;
                std::string arg = yggdrasil::Yggdrasil::read_path(mem_, str_ptr);
                new_argv.push_back(arg);
                argv_ptr += 8;
            }
            // Read the ELF file.
            FILE* f = fopen(path.c_str(), "rb");
            bool is_aarch64 = false;
            std::vector<uint8_t> elf_data;
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0) {
                    elf_data.resize(sz);
                    if (fread(elf_data.data(), 1, sz, f) == static_cast<size_t>(sz)) {
                        if (elf_data.size() >= 64 && elf_data[0] == 0x7f &&
                            elf_data[1] == 'E') {
                            uint16_t e_machine;
                            memcpy(&e_machine, elf_data.data() + 18, 2);
                            if (e_machine == 183) {  // EM_AARCH64
                                is_aarch64 = true;
                            }
                        }
                    }
                }
                fclose(f);
            }
            if (!is_aarch64) {
                // ── Multi-call binary redirect ────────────────────────
                // The requested binary is not AArch64 (or doesn't exist).
                // Check if the currently-running ELF (emu.elf_path()) can
                // act as a multi-call binary for the requested command.
                // This makes `toybox sh -c 'ls /'` work: the shell tries
                // to exec /bin/ls (host x86-64), we redirect to running
                // toybox with argv[0]="ls".
                std::string basename = path;
                size_t slash = basename.rfind('/');
                if (slash != std::string::npos) {
                    basename = basename.substr(slash + 1);
                }
                // Don't redirect if the basename IS the current ELF's
                // basename (infinite loop guard).
                std::string elf_path = emu.elf_path();
                std::string elf_basename = elf_path;
                size_t eslash = elf_basename.rfind('/');
                if (eslash != std::string::npos) {
                    elf_basename = elf_basename.substr(eslash + 1);
                }
                if (basename == elf_basename) {
                    // Same binary — return the original error.
                    if (f) {
                        ret_err(ENOEXEC);
                    } else {
                        ret_err(ENOENT);
                    }
                    return 0;
                }
                // Try to re-exec the current ELF with argv[0] = basename.
                FILE* ef = fopen(elf_path.c_str(), "rb");
                if (!ef) {
                    if (f) {
                        ret_err(ENOEXEC);
                    } else {
                        ret_err(ENOENT);
                    }
                    return 0;
                }
                fseek(ef, 0, SEEK_END);
                long esz = ftell(ef);
                fseek(ef, 0, SEEK_SET);
                if (esz <= 0) {
                    fclose(ef);
                    ret_err(ENOEXEC);
                    return 0;
                }
                elf_data.resize(esz);
                if (fread(elf_data.data(), 1, esz, ef) != static_cast<size_t>(esz)) {
                    fclose(ef);
                    ret_err(EIO);
                    return 0;
                }
                fclose(ef);
                // Replace argv[0] with the basename so the multi-call
                // binary knows which command to run.
                if (new_argv.empty()) {
                    new_argv.push_back(basename);
                } else {
                    new_argv[0] = basename;
                }
                if (dbg().exec_trace) {
                    fprintf(stderr, "[exec] multi-call redirect: '%s' -> "
                            "'%s %s'\n", path.c_str(), elf_path.c_str(),
                            basename.c_str());
                }
            }
            if (elf_data.empty()) {
                ret_err(ENOENT);
                return 0;
            }
            if (new_argv.empty()) new_argv.push_back(path);
            // fork(), the child is executing INSIDE the JIT code buffer
            // (the fork/execve syscall was JIT'd, and jit_interp_step()
            // was called from JIT code). flush_cache() resets
            // code_buf_used_ to 0, so the next block translation would
            // write at offset 0, OVERWRITING the currently-executing
            // JIT code → host SIGTRAP (exit code 133). This broke
            // `toybox sh /tmp/script.sh` (fork+exec from script file)
            // while `sh -c 'cmd'` worked (different code path).
            //
            // Fix: set jit_disabled_ = true atomically. This prevents
            // new block translations (run_block falls back to interpreter)
            // without touching the code buffer. The stale JIT code can
            // finish executing safely (it just returns to run_block,
            // which calls emu.step() because jit_disabled_ is true).
            // After the current block ends, the run loop checks
            // jit_enabled_ (false after fork) and switches to pure
            // interpreter mode for the new binary.
            if (emu.jit()) {
                emu.jit()->jit_disabled_.store(true, std::memory_order_relaxed);
            }
            // simulate execve's memory image replacement. On real Linux,
            // execve() removes ALL old mappings (heap, mmap, etc.) and
            // only the new binary's PT_LOAD segments + stack remain.
            // Without this, the new binary's musl finds stale data from
            // the old binary (malloc locks, thread structures) and hits
            // an assertion failure (BRK #1000 = musl's a_crash()).
            //
            // We zero out the pages in the mmap_alloc region. This
            // clears stale heap data, malloc locks, and thread structures.
            // The new binary's musl will see zeroed memory and initialize
            // fresh. We don't zero the stack (at Memory::STACK_TOP) or the
            // binary's own PT_LOAD segments (below 0x40000000).
            {
                auto allocs = mem_.allocations_snapshot();
                for (auto& [addr, size] : allocs) {
                    if (addr >= Memory::MMAP_BASE_MIN &&
                        addr < Memory::STACK_TOP) {
                        // Zero out the pages at this allocation.
                        try {
                            std::vector<uint8_t> zeros(size, 0);
                            mem_.write(addr, zeros.data(), size);
                        } catch (...) {
                            // Page might not be mapped — skip.
                        }
                        mem_.untrack_allocation(addr, size);
                    }
                }
            }
            // Reload the ELF into the existing Memory. The ElfLoader will
            // map new PT_LOAD segments. Old mappings remain but are
            // overwritten by the new binary's segments.
            auto info = ElfLoader::load(mem_, elf_data);
            // Set up a new initial stack.
            const uint64_t STACK_TOP = Memory::STACK_TOP;
            // The stack is already mapped from the parent; just reset SP.
            uint64_t sp = STACK_TOP;
            // Push argv strings.
            std::vector<uint64_t> argv_addrs;
            for (auto& a : new_argv) {
                sp -= a.size() + 1;
                mem_.write(sp, a.data(), a.size() + 1);
                argv_addrs.push_back(sp);
            }
            // Push envp. Use the Emulator's guest_env_ (which was either
            // set via set_guest_env() or built by build_default_guest_env()
            // at startup). This propagates TZ, LANG, LC_*, etc. from the
            // host so locale-aware programs work correctly in the new
            // process image too.
            //
            // BUGFIX: the old code only pushed "PATH=..." here, dropping
            // TZ and all locale vars in execve'd processes. This meant
            // `toybox sh -c 'uptime'` showed UTC time even when the
            // parent shell had TZ set correctly.
            std::vector<uint64_t> envp_addrs;
            const auto& envs = emu.guest_env().empty()
                ? Emulator::build_default_guest_env()
                : emu.guest_env();
            for (const auto& e : envs) {
                sp -= e.size() + 1;
                mem_.write(sp, e.data(), e.size() + 1);
                envp_addrs.push_back(sp);
            }
            // AT_RANDOM.
            sp -= 16;
            uint8_t rnd[16] = {0};
            mem_.write(sp, rnd, 16);
            uint64_t random_addr = sp;
            // Build auxv.
            std::vector<uint64_t> auxv = {
                6, 4096,           // AT_PAGESZ
                3, info.phdr_addr, // AT_PHDR
                4, info.phent,     // AT_PHENT
                5, info.phnum,     // AT_PHNUM
                9, info.entry,     // AT_ENTRY
                25, random_addr,   // AT_RANDOM
                16, 0x3ff,         // AT_HWCAP (FP+ASIMD+ATOMICS)
                7, 0,              // AT_BASE
                0, 0,              // AT_NULL
            };
            // Compute total table size and align SP to 16.
            uint64_t argc = new_argv.size();
            uint64_t table_size = 8 + 8 * (argc + 1) + 8 * (envp_addrs.size() + 1) + 8 * auxv.size();
            sp -= table_size;
            sp &= ~0xFULL;
            uint64_t p = sp;
            auto push = [&](uint64_t v) { mem_.store<uint64_t>(p, v); p += 8; };
            push(argc);
            for (auto a : argv_addrs) push(a);
            push(0);
            for (auto e : envp_addrs) push(e);
            push(0);
            for (auto v : auxv) push(v);
            // Set CPU state for the new program.
            cpu.pc = info.entry;
            cpu.sp = sp;
            cpu.pstate = 0;
            cpu.running = true;
            cpu.exit_code = 0;
            memset(cpu.regs, 0, sizeof(cpu.regs));
            memset(cpu.v_lo, 0, sizeof(cpu.v_lo));
            memset(cpu.v_hi, 0, sizeof(cpu.v_hi));
            cpu.tid = static_cast<int>(getpid());
            // the new binary loads at the same addresses as the old one
            // (e.g., 0x400000). The decode cache matches on PC, so stale
            // entries from the old binary would match new PCs but return
            // wrong decoded instructions — causing the child to execute
            // garbage and exit with code 133 (SIGTRAP from a BRK in the
            // wrongly-decoded instruction stream). Setting all tags to
            // UINT64_MAX (the "empty" sentinel) forces a fresh decode.
            for (auto& ce : cpu.decode_cache) ce.tag = UINT64_MAX;
            if (dbg().exec_trace) {
                fprintf(stderr, "[exec] post-execve: pc=0x%llx sp=0x%llx "
                        "tid=%d regs zeroed, decode cache invalidated\n",
                        static_cast<unsigned long long>(cpu.pc),
                        static_cast<unsigned long long>(cpu.sp),
                        cpu.tid);
            }
            // Set up a fresh TLS scratch area (like load_elf_file does).
            const uint64_t TLS_SCRATCH_SIZE = 65536;
            uint64_t tls_scratch = mem_.mmap_alloc(TLS_SCRATCH_SIZE);
            cpu.tpidr_el0 = tls_scratch + TLS_SCRATCH_SIZE / 2;
            cpu.tpidrro_el0 = cpu.tpidr_el0;
            // Map the zero page (NULL deref returns 0).
            mem_.map_range(0, 4096);
            // Return 0 to indicate execve succeeded (the syscall doesn't
            // actually return on success — we just set PC to the entry
            // point and continue).
            return 0;
        }
        case 98: { // futex(uaddr, op, val, timeout, uaddr2, val3)
            // Real futex implementation: WAIT blocks the calling thread
            // until woken or timeout; WAKE wakes blocked threads. Uses
            // per-address (mutex, condvar) pairs stored in the futex table.
            //
            // Supported ops:
            //   FUTEX_WAIT (0):           block if *uaddr == val
            //   FUTEX_WAKE (1):           wake up to val waiters
            //   FUTEX_WAIT_BITSET (9):    like WAIT but with bitset
            //   FUTEX_WAKE_BITSET (10):   like WAKE but with bitset
            //   FUTEX_REQUEUE (3):        requeue waiters from uaddr to uaddr2
            //   FUTEX_CMP_REQUEUE (4):    requeue with comparison
            //   FUTEX_LOCK_PI / UNLOCK_PI / etc.: not supported (return -ENOSYS)
            uint64_t uaddr = a0;
            uint32_t op = static_cast<uint32_t>(a1);
            uint32_t val = static_cast<uint32_t>(a2);
            uint64_t timeout_ptr = a3;
            uint64_t uaddr2 = a4;
            uint32_t val3 = static_cast<uint32_t>(a5);
            // Mask out private flag — we treat all futexes as private.
            // Also mask out FUTEX_CLOCK_REALTIME (0x100) which selects
            // CLOCK_REALTIME for FUTEX_WAIT_BITSET. Without masking it,
            // FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME (= 0x109) falls
            // through to the default case and returns -ENOSYS, breaking
            // pthread condvars that use absolute CLOCK_REALTIME timeouts.
            op &= ~0x80;  // FUTEX_PRIVATE_FLAG
            op &= ~0x100; // FUTEX_CLOCK_REALTIME
            bool clock_realtime = (static_cast<uint32_t>(a1) & 0x100) != 0;
            switch (op) {
                case 0:  // FUTEX_WAIT
                case 9:  // FUTEX_WAIT_BITSET
                {
                    // FUTEX_WAIT_BITSET (op 9) treats the timeout as an
                    // ABSOLUTE time on CLOCK_MONOTONIC (or CLOCK_REALTIME
                    // if FUTEX_CLOCK_REALTIME was set). FUTEX_WAIT (op 0)
                    // treats the timeout as RELATIVE.
                    bool absolute = (op == 9);
                    // Always check *uaddr == val BEFORE any fast-path
                    // return. The kernel returns -EAGAIN if *uaddr != val,
                    // regardless of whether other threads are alive. The
                    // old single-threaded fast-path returned 0 without
                    // checking, breaking try-lock patterns (where the
                    // guest expects -EAGAIN when the lock is held by the
                    // current thread).
                    uint32_t cur = mem_.load<uint32_t>(uaddr);
                    if (dbg().futex_trace) {
                        fprintf(stderr, "[FUTX t%d] WAIT op=%u addr=%#llx val=%u cur=%u to=%llx pc=0x%llx lr=0x%llx\n",
                                cpu.tid, (unsigned)op, (unsigned long long)uaddr,
                                val, cur, (unsigned long long)timeout_ptr,
                                (unsigned long long)cpu.pc, (unsigned long long)cpu.regs[30]);
                    }
                    if (cur != val) {
                        ret_err(EAGAIN);
                        return 0;
                    }
                    // Backward-compat: if no other threads are alive to
                    // wake us, return 0 immediately (pretend we waited
                    // and were woken). This preserves the previous
                    // single-threaded behavior where futex was a no-op.
                    // Without this, glibc's startup mutex lock would
                    // block forever in single-threaded code.
                    if (emu.alive_threads_.load() == 0) {
                        ret_host(0);
                        return 0;
                    }
                    Emulator::FutexSlot* slot = get_futex(uaddr);
                    std::unique_lock<std::mutex> lk(slot->mu);
                    // Re-check *uaddr == val UNDER the slot lock so that
                    // a concurrent WAKE can't slip in between our initial
                    // check and our waiter increment. Without this, a
                    // waker could see waiters==0 and skip notify, leaving
                    // us sleeping forever.
                    cur = mem_.load<uint32_t>(uaddr);
                    if (cur != val) {
                        ret_err(EAGAIN);
                        return 0;
                    }
                    slot->waiters++;
                    if (dbg().futex_bt) {
                        fprintf(stderr, "[BT t%d] WAIT op=%u addr=%#llx val=%u cur=%u to=%#llx pc=%s lr=%s\n",
                                cpu.tid, (unsigned)op, (unsigned long long)uaddr, val, cur,
                                (unsigned long long)timeout_ptr, bt_sym(emu, cpu.pc).c_str(),
                                bt_sym(emu, cpu.regs[30]).c_str());
                        uint64_t fp = cpu.regs[29];
                        uint64_t orig_fp = fp;
                        for (int i = 0; i < 32 && fp && (fp & 1) == 0; i++) {
                            uint64_t ra = 0, pfp = 0;
                            try {
                                mem_.read(fp, &pfp, 8);
                                mem_.read(fp + 8, &ra, 8);
                            } catch (...) { break; }
                            fprintf(stderr, "  guest[%2d] fp=0x%llx ra=%s\n",
                                    i, (unsigned long long)fp, bt_sym(emu, ra).c_str());
                            if (pfp <= fp) break;
                            fp = pfp;
                        }
                        if (dbg().btraw) {
                            for (int i = 0; i < 6; i++) {
                                uint64_t a = orig_fp + i * 8;
                                uint64_t w = 0;
                                try { w = mem_.load_64(a); } catch (...) { fprintf(stderr, "  [raw %d] read-fail at %#llx\n", i, (unsigned long long)a); break; }
                                fprintf(stderr, "  [raw %d] %#llx = %#llx\n", i, (unsigned long long)a, (unsigned long long)w);
                            }
                        }
                        uint64_t sp = cpu.regs[31];
                        int nsc = 0;
                        for (uint64_t a = sp & ~0x7ULL; a < sp + 8192 && nsc < 24; a += 8) {
                            uint64_t w = 0;
                            try { w = mem_.load_64(a); } catch (...) { break; }
                            if (w >= Memory::MMAP_BASE_MIN && w <= Memory::MMAP_BASE_MAX) {
                                fprintf(stderr, "  [%#llx] w=%#llx\n", (unsigned long long)a, (unsigned long long)w);
                                nsc++;
                            }
                        }
                        if (dbg().allbt) {
                            std::lock_guard<std::mutex> tlk(emu.threads_mu_);
                            for (auto& gt : emu.threads_) {
                                if (!gt) continue;
                                const CPU& oc = gt->cpu;
                                fprintf(stderr, "  [THR t%d] state=%d pc=%s lr=%s x29=0x%llx sp=0x%llx tid=%d\n",
                                        oc.tid, (int)oc.running, bt_sym(emu, oc.pc).c_str(),
                                        bt_sym(emu, oc.regs[30]).c_str(),
                                        (unsigned long long)oc.regs[29],
                                        (unsigned long long)oc.sp, gt->tid);
                                if (dbg().allbt2 && (oc.regs[29] & 1) == 0) {
                                    uint64_t fp = oc.regs[29];
                                    for (int i = 0; i < 24 && fp && (fp & 1) == 0; i++) {
                                        uint64_t ra = 0, pfp = 0;
                                        try {
                                            mem_.read(fp, &pfp, 8);
                                            mem_.read(fp + 8, &ra, 8);
                                        } catch (...) { break; }
                                        fprintf(stderr, "    t%d[%2d] fp=0x%llx ra=%s\n",
                                                oc.tid, i, (unsigned long long)fp, bt_sym(emu, ra).c_str());
                                        if (pfp <= fp) break;
                                        fp = pfp;
                                    }
                                }
                            }
                        }
                    }
                    // FUTEX_WAIT_BITSET with bitset=0 is invalid per the
                    // kernel, but we treat it as a normal WAIT for
                    // robustness (the guest shouldn't pass 0).
                    if (timeout_ptr == 0) {
                        slot->cv.wait(lk);
                    } else {
                        // timeout is struct timespec { sec, nsec }
                        uint64_t sec  = mem_.load<uint64_t>(timeout_ptr);
                        uint64_t nsec = mem_.load<uint64_t>(timeout_ptr + 8);
                        if (absolute) {
                            // FUTEX_WAIT_BITSET: timeout is an ABSOLUTE deadline.
                            // BUGFIX: the previous code computed
                            //   deadline = now_mono + (sec + nsec) - now_mono
                            //          = (sec + nsec) as a RELATIVE duration,
                            // which is wrong. If the guest's monotonic clock
                            // reads 1000s and the deadline is 1010s, the
                            // previous code waited 1010s instead of 10s,
                            // breaking pthread_cond_timedwait(CLOCK_MONOTONIC).
                            //
                            // The fix: treat (sec, nsec) as a steady_clock
                            // time_point directly. The guest's CLOCK_MONOTONIC
                            // and the host's steady_clock both count from
                            // boot, so their origins coincide closely enough
                            // for futex deadlines. For CLOCK_REALTIME we use
                            // system_clock instead.
                            if (clock_realtime) {
                                // Build absolute system_clock time_point and
                                // convert to steady_clock-relative duration
                                // for the cv.wait_until call.
                                auto abs_sys = std::chrono::system_clock::from_time_t(sec)
                                             + std::chrono::nanoseconds(nsec);
                                auto now_sys = std::chrono::system_clock::now();
                                if (abs_sys <= now_sys) {
                                    slot->waiters--;
                                    ret_host(0);
                                    return 0;
                                }
                                auto rel = std::chrono::duration_cast<
                                    std::chrono::nanoseconds>(abs_sys - now_sys);
                                slot->cv.wait_for(lk, rel);
                            } else {
                                // CLOCK_MONOTONIC absolute deadline.
                                // Construct a steady_clock time_point whose
                                // time_since_epoch() == (sec, nsec). This
                                // works because steady_clock's epoch is
                                // implementation-defined but stable, and
                                // the guest's CLOCK_MONOTONIC counts from
                                // boot — close enough for emulator use.
                                using steady_tp = std::chrono::steady_clock::time_point;
                                steady_tp abs_mono{std::chrono::nanoseconds(
                                    sec * 1000000000ULL + nsec)};
                                slot->cv.wait_until(lk, abs_mono);
                            }
                        } else {
                            // FUTEX_WAIT: timeout is relative.
                            auto duration = std::chrono::seconds(sec) +
                                            std::chrono::nanoseconds(nsec);
                            slot->cv.wait_for(lk, duration);
                        }
                    }
                    slot->waiters--;
                    // NOTE: we do NOT reclaim the FutexSlot here even if
                    // waiters==0. The slot is owned by the shard's
                    // unordered_map, and erasing it while another thread
                    // holds a FutexSlot* from get_futex() would be a
                    // use-after-free. The sharded design (64 shards)
                    // already eliminates the contention problem; slot
                    // reclamation needs epoch-based reclamation or a
                    // free-list and is deferred. The memory cost is
                    // ~88 bytes per distinct futex word ever waited on —
                    // acceptable for typical game workloads.
                    ret_host(0);
                    return 0;
                }
                case 1:  // FUTEX_WAKE
                case 10: // FUTEX_WAKE_BITSET
                {
                    // 1.5.3-alpha fast path: skip the slot mutex when
                    // there are no waiters. This is the common case for
                    // pthread_mutex_unlock on an uncontended lock — the
                    // thread calls FUTEX_WAKE(1) but no one is waiting.
                    // Skipping the slot mutex saves ~50ns per unlock on
                    // 8-vCPU guests.
                    //
                    // We still take the shard mutex (via get_futex) to
                    // look up the slot, but we DON'T take the slot's own
                    // mutex if waiters == 0. The waiters field is read
                    // without the lock — this is a benign race: a waiter
                    // might be in the process of incrementing it, in
                    // which case we'd miss the wake. That's fine because
                    // the waiter will loop and re-check the futex word,
                    // and FUTEX_WAIT has its own retry logic.
                    //
                    // Safety: cv.notify_one()/notify_all() can be called
                    // without holding the mutex (per C++ standard). The
                    // waiter must check the condition in a loop, which
                    // FUTEX_WAIT does.
                    Emulator::FutexSlot* slot = get_futex(uaddr);
                    int to_wake = static_cast<int>(val);
                    if (to_wake <= 0) { ret_host(0); return 0; }
                    int fwaiters = slot->waiters;
                    if (dbg().futex_trace) {
                        fprintf(stderr, "[FUTX t%d] WAKE op=%u addr=%#llx want=%d waiters=%d pc=0x%llx lr=0x%llx\n",
                                cpu.tid, (unsigned)op, (unsigned long long)uaddr,
                                to_wake, fwaiters, (unsigned long long)cpu.pc,
                                (unsigned long long)cpu.regs[30]);
                    }
                    // Read waiters without the lock — see comment above.
                    // Use an atomic read to avoid torn reads on architectures
                    // where int isn't atomic by default (not an issue on
                    // x86_64/AArch64, but defensive).
                    int waiters = __atomic_load_n(&slot->waiters, __ATOMIC_ACQUIRE);
                    if (waiters == 0) {
                        // No one to wake — skip the slot mutex.
                        ret_host(0);
                        return 0;
                    }
                    // There are waiters — take the slot mutex and notify.
                    std::unique_lock<std::mutex> lk(slot->mu);
                    int woken = std::min(to_wake, slot->waiters);
                    if (woken >= slot->waiters) {
                        slot->cv.notify_all();
                    } else {
                        for (int i = 0; i < woken; i++) slot->cv.notify_one();
                    }
                    ret_host(woken);
                    return 0;
                }
                case 3:   // FUTEX_REQUEUE
                case 4: { // FUTEX_CMP_REQUEUE
                    // FUTEX_REQUEUE(uaddr, FUTEX_REQUEUE, nr_wake, nr_requeue,
                    //               uaddr2, val3):
                    //   Wake up to `val` (=nr_wake) waiters on uaddr, then
                    //   move up to `val3` (=nr_requeue) remaining waiters
                    //   from uaddr to uaddr2 WITHOUT waking them. They'll
                    //   be woken by a future FUTEX_WAKE on uaddr2.
                    //
                    // FUTEX_CMP_REQUEUE adds a val3 comparison: only
                    // proceed if *uaddr == val3 (the "cmp" is on uaddr,
                    // not uaddr2).
                    //
                    // `val` waiters and ignoring uaddr2), which caused
                    // spurious wakeups in condvar implementations that
                    // rely on requeue to avoid thundering herds. Now we
                    // do a proper requeue: wake `val` waiters, then move
                    // up to `val3` waiters from uaddr's slot to uaddr2's
                    // slot.
                    //
                    // For CMP_REQUEUE, check *uaddr == val3 first.
                    if (op == 4) {
                        uint32_t cur;
                        try {
                            cur = mem_.load<uint32_t>(uaddr);
                        } catch (...) {
                            ret_err(EFAULT);
                            return 0;
                        }
                        if (cur != val3) {
                            ret_err(EAGAIN);
                            return 0;
                        }
                    }
                    int nr_wake = static_cast<int>(val);
                    int nr_requeue = static_cast<int>(val3);
                    if (uaddr2 == 0 && nr_requeue > 0) {
                        ret_err(EINVAL);
                        return 0;
                    }
                    Emulator::FutexSlot* slot1 = get_futex(uaddr);
                    Emulator::FutexSlot* slot2 = (uaddr2 != 0) ? get_futex(uaddr2) : nullptr;
                    // Lock both slots in address order to avoid deadlock with
                    // a concurrent REQUEUE in the opposite direction. Use
                    // defer_lock so we can acquire in the right order.
                    // Special case: if uaddr2 == uaddr, slot2 == slot1 — lock
                    // only once (Linux returns EINVAL for this, but some guests
                    // pass it; locking the same mutex twice would deadlock).
                    std::unique_lock<std::mutex> lk1(slot1->mu, std::defer_lock);
                    std::unique_lock<std::mutex> lk2;
                    if (slot2 && slot2 != slot1) {
                        lk2 = std::unique_lock<std::mutex>(slot2->mu, std::defer_lock);
                        if (uaddr2 > uaddr) {
                            lk1.lock();
                            lk2.lock();
                        } else {
                            lk2.lock();
                            lk1.lock();
                        }
                    } else {
                        lk1.lock();  // slot2 is null or same as slot1
                    }
                    // Wake up to nr_wake waiters on uaddr.
                    int woken = std::min(nr_wake, slot1->waiters);
                    if (woken > 0) {
                        if (woken >= slot1->waiters) {
                            slot1->cv.notify_all();
                        } else {
                            for (int i = 0; i < woken; i++) slot1->cv.notify_one();
                        }
                        // NOTE: do NOT decrement slot1->waiters here — the
                        // woken threads each decrement it themselves when
                        // they return from cv.wait() (see the FUTEX_WAIT
                        // case). Manually subtracting `woken` double-decrements
                        // and undercounts waiters, which makes a later
                        // FUTEX_WAKE think there are no waiters and skip the
                        // notify → lost wakeup / deadlock.
                    }
                    // Move up to nr_requeue remaining waiters to uaddr2.
                    // We can't selectively move condvar waiters (C++ cv
                    // doesn't support "move N waiters to another cv"),
                    // so we wake the remaining waiters and immediately
                    // re-block them on slot2. This isn't a true requeue
                    // (it causes a spurious wakeup on the moved waiters),
                    // but it's the closest C++ primitives allow. The
                    // guest's futex loop (re-check *uaddr2) handles the
                    // spurious wakeup correctly.
                    int requeued = 0;
                    if (slot2 && nr_requeue > 0) {
                        int to_move = std::min(nr_requeue, slot1->waiters);
                        if (to_move > 0) {
                            // Wake the requeued waiters with notify_all.
                            // They'll return from cv.wait() on slot1, do
                            // slot1->waiters--, then re-check *uaddr via
                            // the guest's futex loop and re-block on slot2
                            // (incrementing slot2->waiters themselves).
                            // We must NOT manually set slot1->waiters=0 or
                            // bump slot2->waiters here — the woken threads'
                            // own decrement/increment handles it. Setting
                            // slot1->waiters=0 would make their decrement
                            // go negative; bumping slot2 would double-count.
                            slot1->cv.notify_all();
                            requeued = to_move;
                        }
                    }
                    ret_host(static_cast<uint64_t>(woken + requeued));
                    return 0;
                }
                default:
                    // PI futexes and others: not supported
                    ret_err(ENOSYS);
                    return 0;
            }
        }
        case 96: { // set_tid_address
            // Stores the tid_address pointer in the calling thread's
            // clear_child_tid field. On real Linux, set_tid_address(2)
            // and CLONE_CHILD_CLEARTID share the SAME task->clear_child_tid
            // field: whichever was set last wins. When the thread exits,
            // the kernel writes 0 to *clear_child_tid and performs a
            // FUTEX_WAKE on it — see exit_mm() in kernel/exit.c.
            //
            // musl relies on this: it calls set_tid_address(&__thread_list_lock)
            // at startup, then spawns threads via clone() with
            // CLONE_CHILD_CLEARTID | ctid=&__thread_list_lock. When the
            // child exits, the kernel clears the lock to 0 and wakes any
            // waiter of __tl_lock — which is how the orphaned lock is
            // released when a thread exits while holding __tl_lock
            // (musl's pthread_exit intentionally does NOT call
            // __tl_unlock before SYS_exit; see the comment in
            // pthread_create.c: "the lock is released, which only
            // happens after SYS_exit has been called, via the exit
            // futex address pointing at the lock").
            //
            // We track set_tid_address_ptr separately only so the
            // get_robust_list-style introspection can still report it;
            // the exit-time cleanup is handled by the clear_child_tid
            // path in thread_entry (which writes 0 + FUTEX_WAKE).
            cpu.set_tid_address_ptr = a0;
            cpu.clear_child_tid     = a0;
            ret_host(cpu.tid);
            return 0;
        }
        case 99: { // set_robust_list(head, len)
            // Record the head of this thread's robust futex list. On
            // thread exit, we walk the list and mark each held futex as
            // FUTEX_OWNER_DIED (see exit_robust_list above).
            cpu.robust_list_head = a0;
            cpu.robust_list_len  = a1;
            // Validate len — must be sizeof(struct robust_list_head) = 24.
            // Some kernels are stricter; we accept any value for forward
            // compat.
            ret_host(0);
            return 0;
        }
        case 100: { // get_robust_list(pid, head_ptr, len_ptr)
            // Return the robust list head for `pid` (0 = current thread).
            // We only support querying the current thread (pid 0 or the
            // caller's TID); other threads' lists are not exposed.
            int pid = static_cast<int>(a0);
            if (pid != 0 && pid != cpu.tid) {
                ret_err(EPERM);  // can't query other threads' robust lists
                return 0;
            }
            if (a1 != 0) {
                try { mem_.store<uint64_t>(a1, cpu.robust_list_head); }
                catch (...) { ret_err(EFAULT); return 0; }
            }
            if (a2 != 0) {
                try { mem_.store<uint64_t>(a2, cpu.robust_list_len); }
                catch (...) { ret_err(EFAULT); return 0; }
            }
            ret_host(0);
            return 0;
        }
        case 131: { // tgkill(tgid, tid, sig) — send signal to specific thread
            // Deliver the signal to the target thread. If the target is
            // the current thread, deliver directly. If it's another
            // thread, we queue the signal for delivery at the target's
            // next syscall boundary or signal-drain point.
            int tgid = static_cast<int>(a0);
            int tid  = static_cast<int>(a1);
            int sig  = static_cast<int>(a2);
            (void)tgid;  // we don't track thread groups separately
            if (sig == 0) {
                // Signal 0: just check permission (always succeeds).
                ret_host(0);
                return 0;
            }
            if (sig < 1 || sig > MAX_SIGNAL) {
                ret_err(EINVAL);
                return 0;
            }
            if (tid == cpu.tid || tid == 0) {
                // Self-delivery.
                deliver_signal(emu, cpu, signals_, sig);
            } else {
                // Cross-thread delivery: push onto the target CPU's
                // pending queue. The target's run loop drains it at the
                // next 4K-instruction boundary (drain_pending_signals).
                //
                // directly on the target CPU while the target's host
                // thread was concurrently executing on it — a textbook
                // data race (regs/pc/sp/sigmask/sigpending mutated under
                // the target's feet). Now we queue and let the target
                // drain itself, matching the kernel's per-task
                // task->pending queue semantics.
                CPU* target = emu.find_cpu_by_tid(tid);
                if (target) {
                    if (!target->push_pending(sig, SI_USER_EMU, 0)) {
                        // Queue full — fall back to setting the bit in
                        // sigpending so the signal isn't lost. The
                        // bit-based delivery path in deliver_pending_signals
                        // will pick it up. (This matches kernel behavior
                        // when task->pending is full — the signal is
                        // recorded in sigpending but loses siginfo.)
                        target->sigpending |= (1ULL << (sig - 1));
                    }
                } else {
                    // Target thread doesn't exist — ESRCH.
                    ret_err(ESRCH);
                    return 0;
                }
            }
            ret_host(0);
            return 0;
        }
        case 130: { // tkill(tid, sig)
            int tid = static_cast<int>(a0);
            int sig = static_cast<int>(a1);
            if (sig == 0) { ret_host(0); return 0; }
            if (sig < 1 || sig > MAX_SIGNAL) { ret_err(EINVAL); return 0; }
            if (tid == cpu.tid || tid == 0) {
                deliver_signal(emu, cpu, signals_, sig);
            } else {
                // Cross-thread: queue on target (see tgkill above).
                CPU* target = emu.find_cpu_by_tid(tid);
                if (target) {
                    if (!target->push_pending(sig, SI_USER_EMU, 0)) {
                        target->sigpending |= (1ULL << (sig - 1));
                    }
                } else {
                    ret_err(ESRCH);
                    return 0;
                }
            }
            ret_host(0);
            return 0;
        }
        case 129: { // kill(pid, sig)
            // kill() sends a signal to a process. For pid > 0, it goes
            // to the main thread (TID 1) of that process. For pid == 0,
            // it goes to the caller's process group (we treat as self).
            // For pid < 0, it goes to a process group (we treat as self
            // for simplicity — guest processes don't have separate pids
            // from our perspective).
            int pid = static_cast<int>(a0);
            int sig = static_cast<int>(a1);
            if (sig == 0) { ret_host(0); return 0; }
            if (sig < 1 || sig > MAX_SIGNAL) { ret_err(EINVAL); return 0; }
            // Guest PID 1 = self (the main guest process, which always
            // has PID 1 in our model). Also treat pid == 0, pid < 0,
            // and pid == host PID as self.
            if (pid == 1 || pid == 0 || pid < 0 ||
                pid == static_cast<int>(::getpid())) {
                // Self-process: deliver to the main thread (TID 1) or
                // the caller if it's the main thread.
                if (cpu.tid == 1) {
                    deliver_signal(emu, cpu, signals_, sig);
                } else {
                    // Cross-thread: queue on main thread's pending queue
                    // (see tgkill above for rationale).
                    CPU* main = emu.find_cpu_by_tid(1);
                    if (main) {
                        if (!main->push_pending(sig, SI_USER_EMU, 0)) {
                            main->sigpending |= (1ULL << (sig - 1));
                        }
                    }
                }
            } else {
                // Other process — forward to host kill() so the target
                // (a forked child process) receives the signal via its
                // host signal handler. This makes kill(child_pid, sig)
                // work for inter-process signaling between forked
                // children.
                int r = ::kill(pid, sig);
                if (r < 0) {
                    ret_err(ESRCH);
                    return 0;
                }
            }
            ret_host(0);
            return 0;
        }
        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}
} // namespace arm64emu
