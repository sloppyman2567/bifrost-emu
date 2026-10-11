// Synthetic ld-linux shim: the emulator-provided dynamic loader object.
#include "System/Loader/DynamicLinker.hpp"
#include "System/Core/CPU.hpp"
#include "System/Memory/Memory.hpp"
#include "Bifrost/Types.hpp"
#include "Bifrost/Version.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include "System/Loader/DynamicLinker/LinkerInternal.hpp"
namespace arm64emu {
// ── register_ld_linux_shim_ ────────────────────────────────────────────
// Allocate a small data + code page in guest memory and populate it
// with synthetic versions of the symbols glibc's libc.so expects from
// ld-linux (the dynamic linker). Without these, libc's GOT slots for
// _rtld_global_ro, _rtld_global, _dl_argv, etc. stay at 0 (because
// no ld-linux was loaded), and libc crashes when it dereferences them
// during __libc_start_main.
//
// The shim provides:
//   - A writable 4 KiB data page with:
//       - _rtld_global_ro struct (zeroed — glibc reads feature flags
//         from here; all-zero means "no special features", which is
//         safe).
//       - _rtld_global struct (zeroed — glibc reads _dl_ns[0]._ns_nloaded
//         = 0 meaning "no libraries loaded via dlopen", which is fine
//         because we don't support dlopen yet).
//       - Storage for _dl_argv (initially NULL — libc sets it during
//         __libc_start_main).
//       - Storage for __libc_enable_secure (0 = not setuid).
//       - Storage for __pointer_chk_guard (random value — see below).
//       - Storage for _dl_start_args._dl_start_time (0).
//   - An executable 4 KiB code page with tiny ARM64 stub functions:
//       - _dl_find_dso_for_object: returns 0 (not found).
//       - _dl_allocate_tls: returns 0 (no dynamic TLS).
//       - _dl_allocate_tls_init: returns 0.
//       - _dl_deallocate_tls: no-op (RET).
//       - _dl_signal_error: calls abort() (which we resolve via
//         the global symbol table).
//       - _dl_signal_exception: same.
//       - _dl_catch_exception: returns 0 (no exception).
//       - _dl_catch_error: returns 0.
//       - _dl_audit_symbind_alt: no-op.
//       - _dl_audit_preinit: no-op.
//       - _dl_rtld_di_serinfo: returns 0.
//       - _dl_call_fini: no-op.
//       - __tls_get_addr: resolves a TLS index through the calling
//         thread's DTV, including modules loaded after startup.
//       - __tunable_get_val: returns 0 (no tunables).
//       - __nptl_change_stack_perm: no-op.
//
// Each stub is a sequence of ARM64 instructions:
//   mov x0, #0   ; 0xd2800000
//   ret          ; 0xd65f03c0
// (or just `ret` for void functions). They're laid out consecutively
// at the start of the code page, 8 bytes each (2 instructions).
//
// The data page is at shim_base_; the code page is at shim_base_+4096.
// Symbols are registered in the global symbol table so relocations
// resolve to the correct addresses.
bool DynamicLinker::register_ld_linux_shim_() {
    if (shim_base_ != 0) return true;  // already registered
    // Allocate 4 pages: 3 data pages + 1 code page.
    // The extra data pages are needed because glibc's _rtld_global_ro
    // struct is large (~4-8 KiB) and fields like dl_tls_static_size
    // are at high offsets (0x800-0x2000+). With only 1 data page,
    // reads past 0xFFF would hit the code page and get garbage.
    constexpr uint64_t SHIM_SIZE = 16384;       // 4 pages
    constexpr uint64_t DATA_SIZE = 12288;       // 3 data pages
    constexpr uint64_t CODE_PAGE_OFF = 12288;   // code at page 4
    shim_base_ = mem_.mmap_alloc(SHIM_SIZE);
    if (shim_base_ == 0) {
        error_ = "register_ld_linux_shim_: mmap_alloc failed";
        return false;
    }
    // ── Data area (shim_base_ .. shim_base_+DATA_SIZE) ────────────
    // Zero the entire data area (mmap_alloc already does this, but be
    // explicit in case the pages were reused from a previous allocation).
    std::vector<uint8_t> zero(DATA_SIZE, 0);
    mem_.write(shim_base_, zero.data(), DATA_SIZE);
    // Layout (offsets within the data page):
    //   0x000: _rtld_global_ro (glibc reads many fields at offsets up
    //          to ~0x1000+ from this struct; the function-pointer fields
    //          like _dl_signal_error are at low offsets and stay zero so
    //          glibc falls back to internal defaults).
    //   0x100: _rtld_global (256 bytes — glibc reads _dl_ns, _dl_nns).
    //   0x200: _dl_argv (8 bytes — pointer to argv; libc sets this).
    //   0x208: __libc_enable_secure (4 bytes — 0 = not secure).
    //   0x20C: __pointer_chk_guard (8 bytes — random XOR canary).
    //   0x214: _dl_start_args (16 bytes — start time, etc.).
    //   0x224: padding to 0x300.
    //   0x300: function pointer table (8 bytes each, 16 entries):
    //          [0] _dl_find_dso_for_object  → code page + 0
    //          [1] _dl_allocate_tls         → code page + 8
    //          [2] _dl_allocate_tls_init    → code page + 24
    //          [3] _dl_deallocate_tls       → code page + 32
    //          [4] _dl_signal_error         → code page + 40 (abort)
    //          [5] _dl_signal_exception     → code page + 48 (abort)
    //          [6] _dl_catch_exception      → code page + 56
    //          [7] _dl_catch_error          → code page + 64
    //          [8] _dl_audit_symbind_alt    → code page + 72
    //          [9] _dl_audit_preinit        → code page + 80
    //          [10] _dl_rtld_di_serinfo     → code page + 88
    //          [11] _dl_call_fini           → code page + 96
    //          [12] __tls_get_addr          → code page + 104
    //          [13] __tunable_get_val       → code page + 112
    //          [14] __nptl_change_stack_perm→ code page + 120
    //          [15] (reserved)
    //   _rtld_global_ro._dl_signal_error etc. point into this table.
    constexpr uint64_t RTLD_GLOBAL_RO_OFF = 0x000;
    constexpr uint64_t RTLD_GLOBAL_OFF    = 0x100;
    constexpr uint64_t DL_ARGV_OFF        = 0x200;
    constexpr uint64_t LIBC_ENABLE_SECURE_OFF = 0x208;
    constexpr uint64_t POINTER_CHK_GUARD_OFF = 0x20C;
    constexpr uint64_t FPTR_TABLE_OFF     = 0x300;
    // Generate a random __pointer_chk_guard value. glibc uses this as
    // a stack-protector canary; we use /dev/urandom for entropy.
    uint64_t chk_guard = 0;
    FILE* ur = fopen("/dev/urandom", "rb");
    if (ur) {
        if (fread(&chk_guard, sizeof(chk_guard), 1, ur) != 1) chk_guard = 0;
        fclose(ur);
    }
    if (chk_guard == 0) chk_guard = 0xDEADBEEFCAFEBABEULL;
    mem_.store<uint64_t>(shim_base_ + POINTER_CHK_GUARD_OFF, chk_guard);
    // ── Populate _rtld_global_ro TLS fields ──────────────────────────
    // glibc's pthread_create → allocate_stack → _dl_allocate_tls_storage
    // reads GLRO(dl_tls_static_size) and GLRO(dl_tls_static_align) to
    // determine how much memory to allocate for a new thread's TLS block.
    // Without these fields set, _dl_allocate_tls_storage allocates 0
    // bytes and allocate_stack hits `assert(size != 0)`.
    //
    // The exact offset of dl_tls_static_size within struct
    // rtld_global_ro varies by glibc version (it's after the large
    // _dl_ns[DL_NNS] array, typically at offset 0x800-0x1800). Rather
    // than hardcode a version-specific offset, we populate a RANGE of
    // candidate offsets with the correct values. The fields are:
    //   dl_tls_static_nelem  (size_t) — number of TLS elements
    //   dl_tls_static_size   (size_t) — total static TLS size
    //   dl_tls_static_used   (size_t) — used portion
    //   dl_tls_static_surplus(size_t) — surplus for alignment
    //   dl_tls_static_align  (size_t) — alignment
    //   dl_tls_max_dtv_idx   (size_t) — max DTV index (= # TLS modules)
    //
    // We set all size_t slots in the upper data area (offsets 0x400
    // through 0x2FF8 — past the function-pointer table, up to the code
    // page boundary) to the correct TLS size value. For alignment
    // fields, we use 64 (TLS_TCB_ALIGN on AArch64).
    // This is a "spray" approach — it's not surgical, but it's robust
    // across glibc versions. The function pointers at lower offsets
    // (0x000-0x3FF) are NOT overwritten — they remain zero so glibc
    // falls back to internal defaults.
    if (static_tls_size_ > 0) {
        // dl_tls_static_size includes the TLS data + TCB + struct pthread +
        // surplus. glibc's allocate_stack allocates this many bytes per
        // thread at the top of the stack. The TCB (tcbhead_t) is at the
        // END of this region, and struct pthread extends below it.
        //
        // On glibc AArch64, sizeof(struct pthread) ≈ 2304 bytes. We must
        // include this in dl_tls_static_size so glibc allocates enough
        // space for struct pthread + TLS data + TCB.
        //
        // (lib + tcb + main) + 2KB + 16KB surplus. This was too small —
        // glibc's struct pthread (~2.3KB) overflowed into the TLS data
        // area, causing the "got = expected/4" TLS corruption pattern
        // with 8+ threads. Now we add sizeof(struct pthread) explicitly.
        constexpr uint64_t TLS_SURPLUS = 16384;
        // static_tls_size_ now already includes TLS_PRE_TCB_SIZE (the
        // struct pthread slot below TP), so it is added exactly once.
        uint64_t tls_static_size = static_tls_size_ + 2048 + TLS_SURPLUS;
        // Round up to alignment (64 bytes).
        tls_static_size = (tls_static_size + 63) & ~63ULL;
        // Store for later use (post-relocation patching of _rtld_global_ro).
        pending_tls_static_size_ = tls_static_size;
        pending_tls_static_align_ = std::max<uint64_t>(64,
                                                        tls_static_alignment_);
    }
    // ── Code page (shim_base_+CODE_PAGE_OFF .. shim_base_+SHIM_SIZE) ─
    // Each stub is 2 instructions (8 bytes), EXCEPT _dl_allocate_tls
    // which is 4 instructions (16 bytes) because it calls back into
    // the emulator via a bifrost-specific syscall to allocate a real
    // per-thread TLS block (see syscall 0x1001 in Miscellaneous.cpp).
    //
    // Stubs that "abort" actually call abort() — we resolve abort via
    // the global symbol table at registration time (it's a libc symbol
    // that's already indexed). If abort isn't found yet (e.g., shim
    // registered before libc loaded), the abort stubs just BRK #1000
    // (visible crash) instead.
    uint64_t code_base = shim_base_ + CODE_PAGE_OFF;
    // ARM64 instruction encodings (little-endian byte order):
    //   mov x0, #0        → 0xD2800000  (MOVZ x0, #0)
    //   ret               → 0xD65F03C0
    //   brk #1000         → 0xD4207D00  (musl's a_crash)
    //   movz x8, #0x1001  → 0xD2820028  (bifrost TLS-alloc syscall nr)
    //   svc #0            → 0xD4000001
    //   nop               → 0xD503201F
    auto emit_mov_x0_0 = [](std::vector<uint8_t>& v) {
        v.push_back(0x00); v.push_back(0x00); v.push_back(0x80); v.push_back(0xD2);
    };
    auto emit_ret = [](std::vector<uint8_t>& v) {
        v.push_back(0xC0); v.push_back(0x03); v.push_back(0x5F); v.push_back(0xD6);
    };
    auto emit_brk_1000 = [](std::vector<uint8_t>& v) {
        v.push_back(0x00); v.push_back(0x7D); v.push_back(0x20); v.push_back(0xD4);
    };
    auto emit_nop = [](std::vector<uint8_t>& v) {
        v.push_back(0x1F); v.push_back(0x20); v.push_back(0x03); v.push_back(0xD5);
    };
    auto emit_stub_return0 = [&](std::vector<uint8_t>& v) {
        emit_mov_x0_0(v);
        emit_ret(v);
    };
    auto emit_stub_void = [&](std::vector<uint8_t>& v) {
        emit_ret(v);
        emit_nop(v);  // pad to 8 bytes
    };
    auto emit_stub_abort = [&](std::vector<uint8_t>& v) {
        emit_brk_1000(v);
        emit_nop(v);
    };
    // _dl_allocate_tls syscall stub (16 bytes = 4 instructions):
    //   movz x8, #0x1001   ; bifrost TLS-alloc syscall number
    //   svc  #0            ; call the emulator
    //   ret                ; return (x0 = TCB pointer from emulator)
    //   nop                ; pad to 16 bytes (next stub is 16-aligned)
    //
    // This replaces the old "return 0" stub. Returning 0 caused glibc's
    // allocatestack.c to hit `assert (size != 0)` because _dl_allocate_tls
    // returned NULL, and glibc treated that as a zero-size allocation.
    // The real fix is to actually allocate a per-thread TLS block + TCB,
    // copy the static TLS template, and return the TCB pointer. The
    // syscall handler (case 0x1001 in Miscellaneous.cpp) does this.
    auto emit_tls_alloc_stub = [&](std::vector<uint8_t>& v) {
        // movz x8, #0x1001  →  0xD2820028
        // Encoding: D2800000 | (hw<<21) | (imm16<<5) | Rd
        //   hw=0 (no shift), imm16=0x1001, Rd=8 (x8)
        //   = 0xD2800000 | 0 | (0x1001<<5) | 8 = 0xD2820028
        // Little-endian bytes: 28 00 82 D2
        //
        // typo made syscall 0x1001 never fire. Now it fires on every
        // _dl_allocate_tls AND _dl_allocate_tls_init call (both stubs use
        // this emitter). The syscall handler copies lib TLS to
        // [tcb-lib_size, tcb) and ZEROS [tcb, tcb+main_memsz) to clear
        // stale .tbss data on stack-cache reuse. This is safe because
        // _dl_allocate_tls/init run BEFORE create_thread sets TCB fields
        // (tcb, self, stack_guard, etc.) — glibc will overwrite the
        // zeroed TCB-area bytes with correct values afterwards.
        v.push_back(0x28); v.push_back(0x00); v.push_back(0x82); v.push_back(0xD2);
        // svc #0           →  0xD4000001
        v.push_back(0x01); v.push_back(0x00); v.push_back(0x00); v.push_back(0xD4);
        // ret              →  0xD65F03C0
        v.push_back(0xC0); v.push_back(0x03); v.push_back(0x5F); v.push_back(0xD6);
        // nop (pad to 16 bytes)
        emit_nop(v);
    };
    auto emit_stub_syscall = [&](std::vector<uint8_t>& v, uint16_t syscall_id) {
        const uint32_t movz_x8 = 0xD2800008u |
                                 (static_cast<uint32_t>(syscall_id) << 5);
        v.push_back(static_cast<uint8_t>(movz_x8));
        v.push_back(static_cast<uint8_t>(movz_x8 >> 8));
        v.push_back(static_cast<uint8_t>(movz_x8 >> 16));
        v.push_back(static_cast<uint8_t>(movz_x8 >> 24));
        v.push_back(0x01); v.push_back(0x00); v.push_back(0x00); v.push_back(0xD4); // svc
        v.push_back(0xC0); v.push_back(0x03); v.push_back(0x5F); v.push_back(0xD6); // ret
        emit_nop(v);
    };
    std::vector<uint8_t> code;
    code.reserve(256);
    // Stub layout — _dl_allocate_tls and _dl_allocate_tls_init are 16 bytes;
    // all others are 8.
    // Offsets are tracked via named constants so the FPTR table and
    // symbol registrations stay in sync.
    constexpr uint32_t OFF_DSO     = 0;    // _dl_find_dso_for_object
    constexpr uint32_t OFF_TLS     = 8;    // _dl_allocate_tls (16 bytes)
    constexpr uint32_t OFF_TLSINIT = 24;   // _dl_allocate_tls_init (16 bytes)
    constexpr uint32_t OFF_DLERROR = 288;  // dlerror (syscall-backed)
    constexpr uint32_t OFF_TLSFREE = 272;  // _dl_deallocate_tls (syscall-backed)
    constexpr uint32_t OFF_SIGERR  = 48;   // _dl_signal_error
    constexpr uint32_t OFF_SIGEXC  = 56;   // _dl_signal_exception
    constexpr uint32_t OFF_CEXC    = 64;   // _dl_catch_exception
    constexpr uint32_t OFF_CERR    = 72;   // _dl_catch_error
    constexpr uint32_t OFF_SBA     = 80;   // _dl_audit_symbind_alt
    constexpr uint32_t OFF_PREINIT = 88;   // _dl_audit_preinit
    constexpr uint32_t OFF_SERINFO = 96;   // _dl_rtld_di_serinfo
    constexpr uint32_t OFF_FINI    = 104;  // _dl_call_fini
    constexpr uint32_t OFF_TLSADDR = 240;  // __tls_get_addr (syscall-backed)
    constexpr uint32_t OFF_TUNABLE = 120;  // __tunable_get_val
    constexpr uint32_t OFF_STACKPERM = 128; // __nptl_change_stack_perm
    constexpr uint32_t OFF_DLOPEN   = 136; // _dl_open (16 bytes — calls syscall 0x1002)
    constexpr uint32_t OFF_DLSYM    = 152; // _dl_sym (16 bytes — calls syscall 0x1003)
    constexpr uint32_t OFF_DLCLOSE  = 168; // _dl_close (8 bytes — return 0 stub)
    constexpr uint32_t OFF_DLITER   = 176; // dl_iterate_phdr (16 bytes — calls syscall 0x1007)
    constexpr uint32_t OFF_DLADDR   = 192; // dladdr (16 bytes — calls syscall 0x1005)
    // TLSDESC resolver (16 bytes) lives at code.size()==208; OFF_DLFO
    // is placed AFTER it (224) so the two don't collide.
    constexpr uint32_t OFF_DLFO     = 224; // _dl_find_object (16 bytes — calls syscall 0x1008)
    constexpr uint32_t OFF_TLSDESC_DYNAMIC = 256; // dynamic TLSDESC resolver
    // [0] _dl_find_dso_for_object (returns void*)
    //     Returns 0 (not found). glibc's dladdr and _dl_open use this
    //     to find the containing DSO for a given address. Returning 0
    //     makes glibc skip link_map validation, which is safer than
    //     returning a base address that glibc would misinterpret as a
    //     struct link_map*.
    //     The real implementation (find_object_by_addr) is available
    //     via syscall 0x1006 for internal use, but the glibc-visible
    //     _dl_find_dso_for_object stub returns 0 for compatibility.
    emit_stub_return0(code);     // offset 0
    // [1] _dl_allocate_tls (16 bytes — calls syscall 0x1001)
    emit_tls_alloc_stub(code);   // offset 8 (16 bytes)
    // [2] _dl_allocate_tls_init (16 bytes — also calls syscall 0x1001)
    //     _dl_allocate_tls is NOT called). Without this, stale .tbss data
    //     from the previous thread would persist. Both stubs use the same
    //     emit_tls_alloc_stub, so syscall 0x1001 fires for both new and
    //     reused stacks.
    emit_tls_alloc_stub(code);   // offset 24 (16 bytes)
    // [3] _dl_deallocate_tls (void)
    emit_stub_void(code);        // offset 32
    // [4] _dl_signal_error (noreturn)
    emit_stub_abort(code);       // offset 40
    // [5] _dl_signal_exception (noreturn)
    emit_stub_abort(code);       // offset 48
    // [6] _dl_catch_exception (returns int)
    emit_stub_return0(code);     // offset 56
    // [7] _dl_catch_error (returns int)
    emit_stub_return0(code);     // offset 64
    // [8] _dl_audit_symbind_alt (void)
    emit_stub_void(code);        // offset 72
    // [9] _dl_audit_preinit (void)
    emit_stub_void(code);        // offset 80
    // [10] _dl_rtld_di_serinfo (returns int)
    emit_stub_return0(code);     // offset 88
    // [11] _dl_call_fini (void)
    emit_stub_void(code);        // offset 96
    // [12] legacy __tls_get_addr slot (the active syscall-backed stub is
    //      placed after OFF_DLFO to keep every established shim offset stable).
    emit_stub_return0(code);     // offset 104
    // [13] __tunable_get_val (returns int)
    emit_stub_return0(code);     // offset 112
    // [14] __nptl_change_stack_perm (void)
    emit_stub_void(code);        // offset 120
    // [15] _dl_open (16 bytes — calls syscall 0x1002 for dlopen support)
    //     struct at _rtld_global_ro + 368, offset +72. The stub calls
    //     syscall 0x1002 which loads the library via DynamicLinker.
    {
        // movz x8, #0x1002  →  0xD2820048
        code.push_back(0x48); code.push_back(0x00); code.push_back(0x82); code.push_back(0xD2);
        // svc #0           →  0xD4000001
        code.push_back(0x01); code.push_back(0x00); code.push_back(0x00); code.push_back(0xD4);
        // ret              →  0xD65F03C0
        code.push_back(0xC0); code.push_back(0x03); code.push_back(0x5F); code.push_back(0xD6);
        // nop (pad to 16 bytes)
        emit_nop(code);
    }
    // [16] _dl_sym (16 bytes — calls syscall 0x1003 for dlsym support)
    //     dlsym@@GLIBC_2.34 reads *(hook+16) for _dl_sym.
    {
        // movz x8, #0x1003  →  0xD2820068
        code.push_back(0x68); code.push_back(0x00); code.push_back(0x82); code.push_back(0xD2);
        // svc #0           →  0xD4000001
        code.push_back(0x01); code.push_back(0x00); code.push_back(0x00); code.push_back(0xD4);
        // ret              →  0xD65F03C0
        code.push_back(0xC0); code.push_back(0x03); code.push_back(0x5F); code.push_back(0xD6);
        // nop (pad to 16 bytes)
        emit_nop(code);
    }
    // [17] _dl_close (8 bytes — stub: return 0)
    //     dlclose reads *(hook+8). We return 0 (success) without
    //     actually unloading the library. This is safe because:
    //     1. glibc's _dl_open wrapper calls _dl_close internally if it
    //        detects the handle isn't a valid link_map struct (which it
    //        isn't — we return base_addr as the handle, not a link_map*).
    //        Returning 0 makes glibc think cleanup succeeded.
    //     2. The user's dlclose() also goes through this stub. Returning
    //        0 means "success" — the library stays mapped (glibc does
    //        the same for nodelete libraries). This is safe for an
    //        emulator where processes are short-lived.
    //     The close_library() method exists for future use (e.g. if we
    //     implement real link_map support and can distinguish internal
    //     vs. user dlclose calls).
    emit_stub_return0(code);  // offset 168 (OFF_DLCLOSE)
    // [18] dl_iterate_phdr (16 bytes — calls syscall 0x1007)
    //     glibc's dl_iterate_phdr walks the link_map list, which we don't
    //     have. We override it with a stub that calls syscall 0x1007,
    //     which invokes our iterate_phdr() implementation. The stub
    //     passes x0 (callback) and x1 (data) through to the syscall.
    {
        // movz x8, #0x1007  →  0xD28200E8
        code.push_back(0xE8); code.push_back(0x00); code.push_back(0x82); code.push_back(0xD2);
        // svc #0           →  0xD4000001
        code.push_back(0x01); code.push_back(0x00); code.push_back(0x00); code.push_back(0xD4);
        // ret              →  0xD65F03C0
        code.push_back(0xC0); code.push_back(0x03); code.push_back(0x5F); code.push_back(0xD6);
        // nop (pad to 16 bytes)
        emit_nop(code);
    }
    // [19] dladdr (16 bytes — calls syscall 0x1005)
    //     Used by the dlfcn_hook (hook+40). glibc's dladdr@@GLIBC_2.34
    //     checks the hook and calls hook->dladdr. We point hook+40 to
    //     this stub, which calls our dladdr implementation via syscall
    //     0x1005. The syscall fills in the Dl_info struct and returns
    //     1 (found) or 0 (not found).
    {
        // movz x8, #0x1005  →  0xD28200A8
        code.push_back(0xA8); code.push_back(0x00); code.push_back(0x82); code.push_back(0xD2);
        // svc #0           →  0xD4000001
        code.push_back(0x01); code.push_back(0x00); code.push_back(0x00); code.push_back(0xD4);
        // ret              →  0xD65F03C0
        code.push_back(0xC0); code.push_back(0x03); code.push_back(0x5F); code.push_back(0xD6);
        // nop (pad to 16 bytes)
        emit_nop(code);
    }
    // [20] TLSDESC resolver stub (8 bytes).
    //     AArch64 general-dynamic TLS uses TLSDESC: the GOT slot holds a
    //     16-byte descriptor {resolver_fn, arg}. The consumer sequence is:
    //         adrp x0, page
    //         ldr  xr, [x0, #off]   ; xr = desc[0] = resolver fn ptr
    //         add  x0, x0, #off2    ; x0 = &desc[1] (the arg)
    //         blr  xr               ; call resolver(arg) -> TLS offset in x0
    //     glibc's real __tlsdesc_* resolvers are hidden in ld-linux, so
    //     we provide our own: it reads the TP-offset we precomputed into
    //     desc[1] and returns it.
    //     ldr x0, [x0, #8]  →  0xF9400400  (bytes 00 04 40 F9)
    //     ret               →  0xD65F03C0  (bytes C0 03 5F D6)
    {
        const uint32_t tlsdesc_off = (uint32_t)code.size();
        // record the offset in a member for the TLSDESC fixup below.
        tlsdesc_resolver_off_ = tlsdesc_off;
        code.push_back(0x00); code.push_back(0x04); code.push_back(0x40); code.push_back(0xF9);
        code.push_back(0xC0); code.push_back(0x03); code.push_back(0x5F); code.push_back(0xD6);
        emit_nop(code);
        emit_nop(code);  // pad to 16 bytes for alignment
    }
    // [21] _dl_find_object shim (16 bytes — calls syscall 0x1008).
    //     glibc 2.42's libgcc_s _Unwind_Find_FDE resolves _dl_find_object
    //     through the PLT; funnelled glibc additionally forwards the
    //     libc trampoline through _rtld_global_ro.dl_find_object (the
    //     funnel slot patched by patch_rtld_funnel_()). Both paths land
    //     here: the stub passes x0 (pc) and x1 (&struct dl_find_object)
    //     through to syscall 0x1008, which fills the glibc 2.42 result
    //     struct (flags/map_start/map_end/link_map/eh_frame/sframe) from
    //     host LoadedObject state. Returning 0 + a real eh_frame lets
    //     _Unwind_Find_FDE binary-search .eh_frame_hdr so C++ throw/catch
    //     works; the old -1 stub aborted on every exception.
    {
        // movz x8, #0x1008  →  0xD2820108
        code.push_back(0x08); code.push_back(0x01); code.push_back(0x82); code.push_back(0xD2);
        // svc #0           →  0xD4000001
        code.push_back(0x01); code.push_back(0x00); code.push_back(0x00); code.push_back(0xD4);
        // ret              →  0xD65F03C0
        code.push_back(0xC0); code.push_back(0x03); code.push_back(0x5F); code.push_back(0xD6);
        // nop (pad to 16 bytes)
        emit_nop(code);
    }
    if (code.size() != OFF_TLSADDR) {
        error_ = "ld-linux shim TLS stub layout mismatch";
        return false;
    }
    // __tls_get_addr receives struct tls_index* in x0.
    emit_stub_syscall(code, 0x1009);
    if (code.size() != OFF_TLSDESC_DYNAMIC) {
        error_ = "ld-linux shim TLSDESC stub layout mismatch";
        return false;
    }
    // The TLSDESC resolver receives the descriptor address in x0.
    tlsdesc_dynamic_resolver_off_ = static_cast<uint32_t>(code.size());
    emit_stub_syscall(code, 0x100A);
    if (code.size() != OFF_TLSFREE) {
        error_ = "ld-linux shim TLS deallocator layout mismatch";
        return false;
    }
    emit_stub_syscall(code, 0x100B);
    if (code.size() != OFF_DLERROR) {
        error_ = "ld-linux shim dlerror stub layout mismatch";
        return false;
    }
    emit_stub_syscall(code, 0x100C);
    dl_find_object_stub_ = code_base + OFF_DLFO;
    // Pad to page size.
    code.resize(4096, 0x1F);  // NOP-fill the rest (0xD503201F LE)
    // Write the code page.
    mem_.write(code_base, code.data(), 4096);
    // ── Populate the function pointer table in the data page ──────
    // _rtld_global_ro has fields at specific offsets that glibc reads
    // to find _dl_signal_error, _dl_catch_error, etc. Rather than
    // replicate the exact struct layout (which varies by glibc
    // version), we point the FPTR table at the code stubs and let
    // _rtld_global_ro's fields be zero (glibc will fall back to
    // internal defaults or skip the call if the field is 0). This is
    // the same approach glibc itself uses when loaded by a non-glibc
    // dynamic linker (e.g., for static-PIE binaries).
    //
    // The FPTR table is mostly for our own bookkeeping — if a future
    // glibc version reads a function pointer from _rtld_global_ro
    // at a specific offset, we can wire it up here.
    uint32_t fptr_offsets[15] = {
        OFF_DSO, OFF_TLS, OFF_TLSINIT, OFF_TLSFREE,
        OFF_SIGERR, OFF_SIGEXC, OFF_CEXC, OFF_CERR,
        OFF_SBA, OFF_PREINIT, OFF_SERINFO, OFF_FINI,
        OFF_TLSADDR, OFF_TUNABLE, OFF_STACKPERM,
    };
    for (int i = 0; i < 15; i++) {
        mem_.store<uint64_t>(shim_base_ + FPTR_TABLE_OFF + i * 8,
                             code_base + fptr_offsets[i]);
    }
    // ── dlopen hook struct ─────────────────────────────────────────
    // glibc's __libc_dlopen_mode reads _dl_open from a hook struct:
    //   1. ldr x2, [rtld_global_ro + 368]  → hook struct pointer
    //   2. ldr x2, [x2 + 72]               → _dl_open function pointer
    //   3. blr x2                          → call _dl_open
    // We allocate a 128-byte hook struct in the shim data area (at
    // offset 0x800, past the FPTR table) and set hook+72 = _dl_open stub.
    // After patch_rtld_global_ro_ resolves _rtld_global_ro's address,
    // we write hook_ptr to rtld_global_ro + 368.
    // ── dlopen hook struct ──────────────────────────────────────────
    // glibc reads _dl_open_hook from _rtld_global_ro + 368.
    // The hook struct has function pointers at various offsets:
    //   +0:  _dl_open  (dlopen@@GLIBC_2.34)
    //   +8:  _dl_close (dlclose@@GLIBC_2.34)
    //   +16: _dl_sym   (dlsym@@GLIBC_2.34)
    //   +72: _dl_open  (__libc_dlopen_mode)
    // The dlfcn_hook struct also has slots for dlvsym(+24), dlerror(+32),
    // dladdr(+40), dladdr1(+48), dlinfo(+56), dlmopen(+64). If these are
    // left as NULL, glibc's dladdr@@GLIBC_2.34 checks hook->dladdr (non-
    // NULL because the hook struct itself is non-NULL) and calls 0x0,
    // crashing. We fill all unused slots with the return-0 stub (OFF_DSO,
    // which is the _dl_find_dso_for_object stub that just returns 0).
    constexpr uint64_t DLOPEN_HOOK_OFF = 0x800;
    constexpr uint64_t DLOPEN_HOOK_SIZE = 128;
    {
        std::vector<uint8_t> zeros(DLOPEN_HOOK_SIZE, 0);
        mem_.write(shim_base_ + DLOPEN_HOOK_OFF, zeros.data(), DLOPEN_HOOK_SIZE);
    }
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 0,  code_base + OFF_DLOPEN);
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 8,  code_base + OFF_DLCLOSE);
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 16, code_base + OFF_DLSYM);
    // Implemented hooks use their syscall-backed stubs; remaining slots
    // use the return-0 stub to avoid calling a null hook pointer.
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 24, code_base + OFF_DSO); // dlvsym
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 32, code_base + OFF_DLERROR); // dlerror: consume pending loader error
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 40, code_base + OFF_DLADDR); // dladdr (real)
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 48, code_base + OFF_DSO); // dladdr1
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 56, code_base + OFF_DSO); // dlinfo
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 64, code_base + OFF_DSO); // dlmopen
    mem_.store<uint64_t>(shim_base_ + DLOPEN_HOOK_OFF + 72, code_base + OFF_DLOPEN);
    dlopen_hook_ptr_ = shim_base_ + DLOPEN_HOOK_OFF;
    // ── dlerror string buffer ──────────────────────────────────────
    // A 256-byte guest buffer for dlerror strings. get_last_error()
    // copies the host-side error string here and returns the guest
    // pointer. Placed at offset 0x900 (past the dlopen hook at 0x800).
    constexpr uint64_t DLERROR_BUF_OFF = 0x900;
    constexpr uint64_t DLERROR_BUF_SZ = 256;
    {
        std::vector<uint8_t> zeros(DLERROR_BUF_SZ, 0);
        mem_.write(shim_base_ + DLERROR_BUF_OFF, zeros.data(), DLERROR_BUF_SZ);
    }
    dlerror_buf_ptr_ = shim_base_ + DLERROR_BUF_OFF;
    // ── Register symbols in the global symbol table ──────────────
    // Data symbols point into the data page; function symbols point
    // into the code page.
    // assigned `symbols_[name] = ...`, which would override a real
    // ld-linux symbol if one was already indexed from .dynsym. Now we
    // only insert if no prior definition exists, matching index_symbols'
    // "first strong wins" semantics.
    auto add_data_sym = [&](const char* name, uint64_t off) {
        if (symbols_.count(name) == 0) {
            symbols_[name] = SymEntry{shim_base_ + off, STB_GLOBAL_};
        }
    };
    auto add_func_sym = [&](const char* name, uint64_t off) {
        if (symbols_.count(name) == 0) {
            symbols_[name] = SymEntry{code_base + off, STB_GLOBAL_};
        }
    };
    // Data symbols (from ld-linux that libc references).
    add_data_sym("_rtld_global_ro", RTLD_GLOBAL_RO_OFF);
    add_data_sym("_rtld_global",    RTLD_GLOBAL_OFF);
    add_data_sym("_dl_argv",        DL_ARGV_OFF);
    add_data_sym("__libc_enable_secure", LIBC_ENABLE_SECURE_OFF);
    add_data_sym("__pointer_chk_guard",  POINTER_CHK_GUARD_OFF);
    // Function symbols (from ld-linux that libc references).
    // Offsets match the fptr_offsets[] table above.
    add_func_sym("_dl_find_dso_for_object", OFF_DSO);
    add_func_sym("_dl_allocate_tls",        OFF_TLS);
    add_func_sym("_dl_allocate_tls_init",   OFF_TLSINIT);
    add_func_sym("_dl_deallocate_tls",      OFF_TLSFREE);
    add_func_sym("_dl_signal_error",        OFF_SIGERR);
    add_func_sym("_dl_signal_exception",    OFF_SIGEXC);
    add_func_sym("_dl_catch_exception",     OFF_CEXC);
    add_func_sym("_dl_catch_error",         OFF_CERR);
    add_func_sym("_dl_audit_symbind_alt",   OFF_SBA);
    add_func_sym("_dl_audit_preinit",       OFF_PREINIT);
    add_func_sym("_dl_rtld_di_serinfo",     OFF_SERINFO);
    add_func_sym("_dl_call_fini",           OFF_FINI);
    add_func_sym("__tls_get_addr",          OFF_TLSADDR);
    add_func_sym("__tunable_get_val",       OFF_TUNABLE);
    add_func_sym("__nptl_change_stack_perm", OFF_STACKPERM);
    // TLS lookup must use our DTV because the native linker does not own
    // glibc's slotinfo list. Route both unversioned and versioned references
    // through the ABI-compatible syscall stub.
    symbols_["__tls_get_addr"] =
        SymEntry{code_base + OFF_TLSADDR, STB_GLOBAL_};
    for (auto& [key, entry] : versioned_symbols_) {
        if (key.rfind("__tls_get_addr@", 0) == 0)
            entry = SymEntry{code_base + OFF_TLSADDR, STB_GLOBAL_};
    }
    symbols_["_dl_deallocate_tls"] =
        SymEntry{code_base + OFF_TLSFREE, STB_GLOBAL_};
    // Override glibc's dl_iterate_phdr with our stub that calls syscall
    // 0x1007. glibc's implementation walks the link_map list, which we
    // don't have. We use FORCE override (not add_func_sym which is
    // first-define-wins) because libc.so.6 already defines
    // dl_iterate_phdr@@GLIBC_2.17.
    symbols_["dl_iterate_phdr"] = SymEntry{code_base + OFF_DLITER, STB_GLOBAL_};
    versioned_symbols_["dl_iterate_phdr@GLIBC_2.17"] = SymEntry{code_base + OFF_DLITER, STB_GLOBAL_};
    // Override glibc's dladdr with our stub that calls syscall 0x1005.
    // glibc's dladdr@@GLIBC_2.34 checks the dlfcn_hook (hook+40, which
    // we already point at OFF_DLADDR) AND may be called directly. We
    // use FORCE override (same pattern as dl_iterate_phdr above) so the
    // symbol resolves to our stub regardless of which call path is used.
    // The stub calls our dladdr() implementation via syscall 0x1005,
    // which fills in the Dl_info struct and returns 1 (found) or 0.
    //
    // History: this override was previously disabled because an earlier
    // attempt crashed glibc startup (the versioned-symbol override
    // conflicted with glibc's internal _dl_addr path). The dlfcn_hook
    // wiring (hook+40 → OFF_DLADDR) was added later and made the
    // user-facing dladdr() work, but the symbol itself stayed
    // unresolved-to-glibc. Re-enabling the symbol override now routes
    // ALL dladdr calls (including _dl_addr's internal use) through our
    // stub, which returns 0 gracefully for unknown addresses — so
    // glibc's internal callers see "not found" and skip link_map
    // validation, which is safe.
    symbols_["dladdr"] = SymEntry{code_base + OFF_DLADDR, STB_GLOBAL_};
    versioned_symbols_["dladdr@GLIBC_2.34"] = SymEntry{code_base + OFF_DLADDR, STB_GLOBAL_};
    versioned_symbols_["dladdr@GLIBC_2.0"] = SymEntry{code_base + OFF_DLADDR, STB_GLOBAL_};
    // _dl_allocate_tls_init with our shim's stubs, even if the real
    // ld-linux already defined them in .dynsym. The real ld-linux's
    // _dl_allocate_tls -> allocate_dtv calls calloc via a function
    // pointer (_rtld_global._dl_calloc / the slot at ld-linux+0x3fb18)
    // that is only populated during ld-linux's own _dl_start startup,
    // which we bypass (we use our own dynamic linker). With that slot
    // NULL, allocate_dtv does `blr x2` with x2=0 -> decode error at
    // pc=0x0. Our shim's _dl_allocate_tls instead traps into the
    // emulator via syscall 0x1001 (see Miscellaneous.cpp case 0x1001), which
    // allocates the per-thread TLS block + TCB, copies the static TLS
    // template, copies the TCB canary fields, and returns the TCB
    // pointer — all without touching ld-linux's uninitialized internal
    // state. _dl_allocate_tls_init returns its argument unchanged
    // (the TLS template copy is already done by the syscall handler).
    //
    // We override BOTH the unversioned table (symbols_) AND the
    // versioned table (versioned_symbols_, keyed "name@GLIBC_PRIVATE")
    // because libc's JUMP_SLOT relocations against these symbols are
    // versioned (@GLIBC_PRIVATE), and resolve_versioned_symbol()
    // consults versioned_symbols_ first. Without the versioned
    // override, libc's PLT call would still resolve to the real
    // ld-linux's _dl_allocate_tls. (Other real ld-linux symbols like
    // _rtld_global remain first-define-wins via add_data_sym above.)
    symbols_["_dl_allocate_tls"]      = SymEntry{code_base + OFF_TLS,     STB_GLOBAL_};
    symbols_["_dl_allocate_tls_init"] = SymEntry{code_base + OFF_TLSINIT, STB_GLOBAL_};
    versioned_symbols_["_dl_allocate_tls@GLIBC_PRIVATE"]      = SymEntry{code_base + OFF_TLS,     STB_GLOBAL_};
    versioned_symbols_["_dl_allocate_tls_init@GLIBC_PRIVATE"] = SymEntry{code_base + OFF_TLSINIT, STB_GLOBAL_};
    // Also register a synthetic LoadedObject so the shim shows up in
    // /proc/self/maps and the allocations_ tracker (for fork safety).
    LoadedObject shim_obj;
    shim_obj.name = "<ld-linux-shim>";
    shim_obj.base_addr = shim_base_;
    shim_obj.is_main = false;
    shim_obj.dyn_addr = 0;  // no PT_DYNAMIC
    objects_.push_back(std::move(shim_obj));
    if (dynlink_trace_enabled()) {
        fprintf(stderr, "[dynlink] registered ld-linux shim: "
                "data @0x%llx, code @0x%llx (15 stubs)\n",
                static_cast<unsigned long long>(shim_base_),
                static_cast<unsigned long long>(code_base));
    }
    return true;
}
}  // namespace arm64emu
