// DynamicLinker.cpp — Bifrost-private host calls 0x1001-0x100C used by the
// ld-linux shim and glibc patches to reach the emulator's dynamic linker:
// TLS allocation, dlopen/dlsym/dlclose/dladdr, _dl_find_dso_for_object,
// dl_iterate_phdr, _dl_find_object, __tls_get_addr, TLSDESC resolution,
// _dl_deallocate_tls and dlerror.
#include "System/Core/Emulator.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/CPU.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Loader/DynamicLinker.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include "System/VFS/VFS.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
namespace arm64emu {
int64_t syscall_linker_services(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    (void)a2;
    auto& mem_ = emu.mem();
    switch (num) {
        // ── Bifrost-emu internal TLS-alloc syscall  ────────
        // Called by _dl_allocate_tls AND _dl_allocate_tls_init stubs.
        //
        //   1. Copies lib TLS template to [tcb-lib_size, tcb) (negative TP offsets)
        //   2. ZEROS [tcb, tcb+main_memsz) to clear stale .tbss data
        //      (this is the main exe TLS area at positive TP offsets)
        //   3. Installs a per-thread DTV in reserved positive TLS slack.
        //      The native linker bypasses ld-linux's DTV allocator, so the
        //      syscall shim must provide this pointer before pthread_create
        //      inspects it. Never copy the main thread's DTV pointer: glibc
        //      would later mistake it for child-owned state.
        //
        // This is SAFE because _dl_allocate_tls/init run BEFORE
        // create_thread. The sequence in glibc's allocate_stack is:
        //   1. Zero struct pthread (new stacks only)
        //   2. Call _dl_allocate_tls OR _dl_allocate_tls_init ← WE FIRE HERE
        //   3. Set pd->start_routine, pd->arg, etc.
        //   4. Call create_thread → sets remaining TCB fields, calls clone3
        //
        // For stack-cache reuse (waves 2+), step 1 is skipped. Our zeroing
        // in step 2 replaces it, clearing stale .tbss data. Glibc's
        // create_thread in step 4 then sets TCB fields, overwriting any
        // zeros we wrote to TCB field offsets.
        //
        // a0 (x0) = `mem` from glibc = TCB pointer (= future TPIDR_EL0).
        //   - non-zero: caller-allocated (normal pthread_create path)
        //   - 0: allocate a new block (rare)
        case 0x1001: {
            if (dbg().dynlink_trace) {
                fprintf(stderr, "[tls-alloc] syscall 0x1001 called: "
                        "a0=0x%llx\n", static_cast<unsigned long long>(a0));
            }
            auto* dl = emu.dyn_linker();
            // Hold the loader lock while accounting TLS sizes and copying
            // per-thread TLS templates — a concurrent dlopen() from another
            // guest thread could otherwise reorder objects_ mid-iteration.
            // (When the linker is absent — static binary, no thunk — the
            // fallback mutex is uncontended and effectively free.)
            std::recursive_mutex fallback_mu;
            std::lock_guard<std::recursive_mutex> tls_lk(
                dl ? dl->loader_lock() : fallback_mu);
            // ── Compute lib_size, main TLS info, and tcb_size ─────────
            // Uses variant-I TLS layout (glibc AArch64):
            //   - Main exe TLS: at POSITIVE TP offsets (TP + tcb_size ..)
            //   - Lib TLS: at NEGATIVE TP offsets (TP - lib_size .. TP)
            //   - TCB header (tcbhead_t): at [TP, TP + tcb_size)
            //
            // See allocate_static_tls() in DynamicLinker.cpp for the
            // full layout description.
            uint64_t lib_size = 0;
            uint64_t main_memsz = 0;
            uint64_t main_align = 1;
            if (dl) {
                for (const auto& obj : dl->objects()) {
                    if (!obj.tls.present || obj.tls.memsz == 0) continue;
                    // Skip dynamic libs: they live in the startup surplus,
                    // outside the frozen TP anchor (mirrors
                    // allocate_thread_tls, which must compute the same TP).
                    if (obj.tls_dynamic) continue;
                    if (obj.is_main) {
                        main_memsz = obj.tls.memsz;
                        main_align = obj.tls.align ? obj.tls.align : 16;
                    } else {
                        uint64_t a = obj.tls.align ? obj.tls.align : 16;
                        lib_size = (lib_size + a - 1) & ~(a - 1);
                        lib_size += obj.tls.memsz;
                    }
                }
                lib_size = (lib_size + 15) & ~15ULL;
            }
            // TLS_TCB_SIZE: matches allocate_static_tls. tcbhead_t is
            // {dtv, private} = 16 bytes per upstream
            // sysdeps/aarch64/nptl/tls.h. Round up to main exe's TLS
            // alignment so local-exec TPREL offsets match.
            constexpr uint64_t TLS_TCB_SIZE_BASE = 0x10;
            uint64_t tcb_size = (main_align > 1)
                ? (TLS_TCB_SIZE_BASE + main_align - 1) & ~(main_align - 1)
                : TLS_TCB_SIZE_BASE;
            uint64_t total_tls_size = lib_size + DynamicLinker::TLS_PRE_TCB_SIZE +
                                      tcb_size + main_memsz;
            uint64_t tcb;
            if (a0 != 0) {
                // Caller-allocated (normal pthread_create path).
                // glibc computes TCB = stack_block + lib_size, so the lib
                // TLS area is [tcb - lib_size, tcb) and main TLS is at
                // [tcb + tcb_size, tcb + tcb_size + main_memsz).
                tcb = a0;
            } else if (total_tls_size == 0) {
                // No TLS — return a minimal zeroed block.
                constexpr uint64_t FALLBACK_SIZE = 4096;
                uint64_t block = mem_.mmap_alloc(FALLBACK_SIZE);
                if (block == 0) { ret_err(ENOMEM); return 0; }
                std::vector<uint8_t> zeros(FALLBACK_SIZE, 0);
                mem_.write(block, zeros.data(), FALLBACK_SIZE);
                tcb = (block + FALLBACK_SIZE - 16) & ~0xFULL;
                ret_host(tcb);
                return 0;
            } else {
                // mem == NULL: allocate a new block.
                // Layout: [block .. block+lib_size) = lib TLS,
                //         [block+lib_size .. block+lib_size+tcb_size) = TCB,
                //         [block+lib_size+tcb_size .. block+total) = main TLS.
                constexpr uint64_t PTHREAD_SLACK = 8192;
                uint64_t alloc_size = total_tls_size + PTHREAD_SLACK;
                alloc_size = (alloc_size + 63) & ~63ULL;
                uint64_t block = mem_.mmap_alloc(alloc_size);
                if (block == 0) { ret_err(ENOMEM); return 0; }
                std::vector<uint8_t> zeros(alloc_size, 0);
                mem_.write(block, zeros.data(), alloc_size);
                tcb = block + lib_size + DynamicLinker::TLS_PRE_TCB_SIZE;  // TCB above struct pthread
            }
            if (dl && a0 != 0) {
                // glibc may recycle a cached TLS block. Drop any emulator-
                // owned dynamic TLS/DTV allocations before resetting it.
                dl->deallocate_thread_tls(mem_, tcb);
            }
            // ── Copy each module's TLS template to its per-thread slot ──
            // Variant-I layout per-thread (TP = tcb):
            //   - lib TLS: [tcb - lib_size, tcb)  (negative TP offset)
            //   - TCB header: [tcb, tcb + tcb_size)  (glibc fills this)
            //   - main TLS: [tcb + tcb_size, tcb + tcb_size + main_memsz)
            //                (positive TP offset)
            //
            // Each object's tls_tp_offset is:
            //   - main: +tcb_size (positive)
            //   - libs: (lib_cursor - lib_size) (negative)
            // So dst = tcb + obj.tls_tp_offset gives the correct per-thread
            // address for both main and lib TLS.
            //
            // negative TP offsets). This broke local-exec TLS access for
            // the main exe — the binary's hardcoded positive TPREL offset
            // (e.g. +0x20) landed in the TCB header instead of the main
            // exe's TLS block. With variant-I, main TLS is at TP + tcb_size
            // (positive), matching the linker's TPREL computation.
            if (dl && total_tls_size > 0) {
                for (const auto& obj : dl->objects()) {
                    if (!obj.tls.present || obj.tls.memsz == 0) continue;
                    if (obj.tls_dynamic && !obj.tls_static_slot) continue;
                    int64_t tp_off = obj.tls_tp_offset;
                    uint64_t dst = static_cast<uint64_t>(
                        static_cast<int64_t>(tcb) + tp_off);
                    uint64_t src = obj.tls_template_addr;
                    uint64_t filesz = obj.tls.filesz;
                    try {
                        std::vector<uint8_t> tls_image(
                            static_cast<size_t>(obj.tls.memsz), 0);
                        if (filesz > 0 && filesz <= obj.tls.memsz) {
                            std::vector<uint8_t> tpl(filesz);
                            mem_.read(src, tpl.data(), filesz);
                            std::copy(tpl.begin(), tpl.end(), tls_image.begin());
                        }
                        mem_.write(dst, tls_image.data(), tls_image.size());
                    } catch (...) {}
                    // Clear the full TLS block first so stack-cache reuse
                    // cannot retain stale .tbss from a prior thread.
                }
            }
            // AArch64 glibc's tcbhead_t is { dtv, private } at TP+0/+8.
            // Keep private clear, and install a real DTV in reserved TLS
            // slack. A null DTV crashes pthread_create's stack-cache path;
            // pointing it into the TLS block also lets glibc inspect and
            // clear its entries without freeing the block itself.
            constexpr uint64_t TCB_PRIVATE_OFFSET = 8;
            try {
                mem_.store<uint64_t>(tcb + TCB_PRIVATE_OFFSET, 0);
            } catch (...) {}
            // A static executable can create threads before its first
            // TLS-bearing dlopen. In that case no variant-I template/DTV
            // exists yet; keep the TCB registered and install its DTV when
            // the first TLS module establishes the layout.
            if (dl && total_tls_size > 0 && dl->static_tls_base() != 0 &&
                !dl->initialize_thread_dtv(mem_, tcb)) {
                ret_err(ENOMEM);
                return 0;
            }
            if (dl && total_tls_size > 0) dl->register_thread_tls_block(tcb);
            if (dbg().dynlink_trace) {
                fprintf(stderr, "[tls-alloc] TCB @0x%llx (total_tls=%llu, "
                        "lib=%llu, main=%llu, %s)\n",
                        static_cast<unsigned long long>(tcb),
                        static_cast<unsigned long long>(total_tls_size),
                        static_cast<unsigned long long>(lib_size),
                        static_cast<unsigned long long>(main_memsz),
                        (a0 != 0) ? "caller-alloc" : "new-block");
            }
            ret_host(tcb);
            return 0;
        }
        // ── Bifrost-emu internal dlopen syscall ──────────
        // Called by the _dl_open stub in the ld-linux shim.
        // a0 (x0) = guest pointer to library path string (null-terminated)
        // a1 (x1) = dlopen mode flags (RTLD_LAZY, RTLD_NOW, etc.)
        // Returns: a handle (>0) on success, 0 on failure.
        case 0x1002: {
            // Read the library path from guest memory.
            std::string path = yggdrasil::Yggdrasil::read_path(mem_, a0);
            if (path.empty()) {
                if (dbg().dynlink_trace) {
                    fprintf(stderr, "[dlopen] empty path\n");
                }
                ret_host(0);
                return 0;
            }
            if (dbg().dynlink_trace) {
                fprintf(stderr, "[dlopen] path='%s' mode=0x%llx\n",
                        path.c_str(), static_cast<unsigned long long>(a1));
            }
            // Load the library via the dynamic linker.
            auto* dl = emu.dyn_linker();
            if (!dl) {
                ret_host(0);
                return 0;
            }
            uint64_t handle = dl->load_library(cpu, path);
            if (handle == 0) {
                if (dbg().dynlink_trace) {
                    fprintf(stderr, "[dlopen] failed: %s\n", dl->error().c_str());
                }
                ret_host(0);
                return 0;
            }
            if (dbg().dynlink_trace) {
                fprintf(stderr, "[dlopen] OK handle=0x%llx\n",
                        static_cast<unsigned long long>(handle));
            }
            ret_host(handle);
            return 0;
        }
        // ── Bifrost-emu internal dlsym syscall ───────────
        // a0 (x0) = dlopen handle (base address of the library)
        // a1 (x1) = guest pointer to symbol name string
        // Returns: symbol address on success, 0 on failure.
        case 0x1003: {
            auto* dl = emu.dyn_linker();
            if (!dl) { ret_host(0); return 0; }
            std::string symname = yggdrasil::Yggdrasil::read_path(mem_, a1);
            if (symname.empty()) {
                if (dbg().dynlink_trace) {
                    fprintf(stderr, "[dlsym] empty symbol name\n");
                }
                dl->set_last_error("empty symbol name");
                ret_host(0);
                return 0;
            }
            // If handle is RTLD_DEFAULT (0) or RTLD_NEXT (-1), search all
            // loaded objects via the global symbol table. Otherwise, use
            // resolve_symbol_in which searches the library's own .dynsym
            // first, then falls back to the global table.
            uint64_t addr = 0;
            if (a0 == 0 || static_cast<int64_t>(a0) == -1) {
                addr = dl->resolve_symbol(symname);
            } else {
                addr = dl->resolve_symbol_in(a0, symname);
            }
            if (addr == 0) {
                dl->set_last_error("symbol '" + symname + "' not found");
            }
            if (dbg().dynlink_trace) {
                fprintf(stderr, "[dlsym] '%s' handle=0x%llx → 0x%llx\n",
                        symname.c_str(),
                        static_cast<unsigned long long>(a0),
                        static_cast<unsigned long long>(addr));
            }
            ret_host(addr);
            return 0;
        }
        // ── Bifrost-emu internal dlclose syscall ──────────
        // a0 (x0) = dlopen handle (base address of the library)
        // Returns: 0 on success, -1 on error (with dlerror set).
        case 0x1004: {
            auto* dl = emu.dyn_linker();
            if (!dl || a0 == 0) { ret_host(static_cast<int64_t>(-1)); return 0; }
            int rc = dl->close_library(cpu, a0);
            if (dbg().dynlink_trace) {
                fprintf(stderr, "[dlclose] handle=0x%llx → %d\n",
                        static_cast<unsigned long long>(a0), rc);
            }
            ret_host(static_cast<int64_t>(rc));
            return 0;
        }
        // ── Bifrost-emu internal dladdr syscall ───────────
        // a0 (x0) = address to look up
        // a1 (x1) = guest pointer to Dl_info struct (4 × uint64 = 32 bytes)
        // Returns: 1 on success (address found), 0 on failure.
        case 0x1005: {
            auto* dl = emu.dyn_linker();
            if (!dl || a0 == 0 || a1 == 0) { ret_host(0); return 0; }
            DynamicLinker::DlInfo info;
            int found = dl->dladdr(a0, info);
            // Write the Dl_info struct to guest memory.
            // struct Dl_info { const char* dli_fname; void* dli_fbase;
            //                 const char* dli_sname; void* dli_saddr; }
            try {
                mem_.store<uint64_t>(a1 + 0,  info.dli_fname);
                mem_.store<uint64_t>(a1 + 8,  info.dli_fbase);
                mem_.store<uint64_t>(a1 + 16, info.dli_sname);
                mem_.store<uint64_t>(a1 + 24, info.dli_saddr);
            } catch (...) { ret_host(0); return 0; }
            if (dbg().dynlink_trace) {
                fprintf(stderr, "[dladdr] addr=0x%llx → found=%d fbase=0x%llx sname=0x%llx\n",
                        static_cast<unsigned long long>(a0), found,
                        static_cast<unsigned long long>(info.dli_fbase),
                        static_cast<unsigned long long>(info.dli_sname));
            }
            ret_host(static_cast<int64_t>(found));
            return 0;
        }
        // ── Bifrost-emu internal _dl_find_dso_for_object syscall ──
        // a0 (x0) = address to find the containing DSO for
        // Returns: the DSO's base address (handle), or 0 if not found.
        // Used by glibc's dladdr and _dl_open to determine the caller's
        // namespace. The old stub returned 0 (not found), which caused
        // dladdr to always fail and _dl_open to skip namespace detection.
        case 0x1006: {
            auto* dl = emu.dyn_linker();
            if (!dl || a0 == 0) { ret_host(0); return 0; }
            const LoadedObject* obj = dl->find_object_by_addr(a0);
            uint64_t base = obj ? obj->base_addr : 0;
            if (dbg().dynlink_trace) {
                fprintf(stderr, "[_dl_find_dso] addr=0x%llx → base=0x%llx (%s)\n",
                        static_cast<unsigned long long>(a0),
                        static_cast<unsigned long long>(base),
                        obj ? obj->name.c_str() : "not found");
            }
            ret_host(base);
            return 0;
        }
        // ── Bifrost-emu internal dl_iterate_phdr syscall ──
        // a0 (x0) = guest callback function pointer
        // a1 (x1) = user data pointer (passed as x2 to callback)
        // Returns: sum of callback return values (matches glibc semantics).
        // The callback is called for each loaded object with:
        //   x0 = pointer to dl_phdr_info struct (64 bytes)
        //   x1 = sizeof(dl_phdr_info) = 64
        //   x2 = data pointer (a1)
        case 0x1007: {
            auto* dl = emu.dyn_linker();
            if (!dl || a0 == 0) { ret_host(0); return 0; }
            int rc = dl->iterate_phdr(cpu, a0, a1);
            if (dbg().dynlink_trace) {
                fprintf(stderr, "[dl_iterate_phdr] callback=0x%llx data=0x%llx → %d\n",
                        static_cast<unsigned long long>(a0),
                        static_cast<unsigned long long>(a1), rc);
            }
            ret_host(static_cast<int64_t>(rc));
            return 0;
        }
        // ── Bifrost-emu internal _dl_find_object syscall ──
        // a0 (x0) = guest address to locate (typically the faulting pc
        //           inside a C++ throw; libgcc_s's _Unwind_Find_FDE calls
        //           _dl_find_object with the pc of the frame it's unwinding)
        // a1 (x1) = guest pointer to a 48-byte struct dl_find_object
        // Returns: 0 on success, -1 if the address isn't in a loaded object.
        // glibc 2.42 dl_find_object layout (from elf/dl-find-object.h):
        //   +0  dlfo_flags   (0 = memory-based .eh_frame_hdr, NOT fd-based;
        //                      the fd path would read() the host fd)
        //   +8  dlfo_map_start
        //   +16 dlfo_map_end
        //   +24 dlfo_link_map
        //   +32 dlfo_eh_frame (absolute .eh_frame_hdr guest address)
        //   +40 dlfo_sframe
        // libgcc_s's _Unwind_Find_FDE checks flags (must not have the
        // DLFO_EH_FRAME_FD bit) then binary-searches dlfo_eh_frame; every
        // C++ throw/catch walks this path. The previous all-zero stub made
        // it return NULL and _Unwind_RaiseException aborted. We return the
        // host LoadedObject's real PT_GNU_EH_FRAME address, which is the
        // actual linker-produced .eh_frame_hdr (version/encodings/table all
        // valid, pcrel/datarel offsets preserved by mapping at load base).
        case 0x1008: {
            auto* dl = emu.dyn_linker();
            if (!dl || a0 == 0 || a1 == 0) { ret_host(-1); return 0; }
            const LoadedObject* obj = dl->find_object_by_addr(a0);
            if (!obj || obj->eh_frame_hdr_addr == 0) {
                if (dbg().dynlink_trace) {
                    fprintf(stderr, "[_dl_find_object] addr=0x%llx → not found"
                            " (obj=%p)\n",
                            static_cast<unsigned long long>(a0), (void*)obj);
                }
                ret_host(-1);
                return 0;
            }
            uint64_t flags = 0, link_map = 0, sframe = 0;
            try {
                mem_.store<uint64_t>(a1 + 0,  flags);
                mem_.store<uint64_t>(a1 + 8,  obj->base_addr);
                mem_.store<uint64_t>(a1 + 16, obj->base_addr + obj->map_size);
                mem_.store<uint64_t>(a1 + 24, link_map);
                mem_.store<uint64_t>(a1 + 32, obj->eh_frame_hdr_addr);
                mem_.store<uint64_t>(a1 + 40, sframe);
            } catch (...) { ret_host(-1); return 0; }
            if (dbg().dynlink_trace) {
                fprintf(stderr, "[_dl_find_object] addr=0x%llx → %s eh_frame=0x%llx"
                        " start=0x%llx end=0x%llx\n",
                        static_cast<unsigned long long>(a0), obj->name.c_str(),
                        static_cast<unsigned long long>(obj->eh_frame_hdr_addr),
                        static_cast<unsigned long long>(obj->base_addr),
                        static_cast<unsigned long long>(obj->base_addr + obj->map_size));
            }
            ret_host(0);
            return 0;
        }
        // ── Bifrost-emu TLS resolver syscalls ───────────────────────
        // AArch64 TLS index layout is { module_id, module_offset }.
        case 0x1009: { // __tls_get_addr
            auto* dl = emu.dyn_linker();
            if (!dl || a0 == 0) { ret_host(0); return 0; }
            uint64_t module_id = 0;
            uint64_t offset = 0;
            try {
                module_id = mem_.load<uint64_t>(a0);
                offset = mem_.load<uint64_t>(a0 + sizeof(uint64_t));
            } catch (...) {
                ret_host(0);
                return 0;
            }
            ret_host(dl->resolve_tls_address(mem_, cpu, module_id, offset));
            return 0;
        }
        case 0x100A: { // dynamic TLSDESC resolver
            auto* dl = emu.dyn_linker();
            if (!dl || a0 == 0) { ret_host(0); return 0; }
            ret_host(static_cast<uint64_t>(
                dl->resolve_tlsdesc_offset(mem_, cpu, a0)));
            return 0;
        }
        case 0x100B: { // _dl_deallocate_tls
            if (auto* dl = emu.dyn_linker())
                dl->deallocate_thread_tls(mem_, a0);
            ret_host(0);
            return 0;
        }
        case 0x100C: { // dlerror: guest string, consumed on first read
            auto* dl = emu.dyn_linker();
            ret_host(dl ? dl->get_last_error() : 0);
            return 0;
        }
        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}
} // namespace arm64emu
