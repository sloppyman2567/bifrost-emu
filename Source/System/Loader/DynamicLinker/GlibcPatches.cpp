// glibc ld.so integration: _rtld_global_ro / funnel patches, offset detection,
// libthread_db descriptors and NPTL thread-list initialization.
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
// ── patch_rtld_global_ro_ ──────────────────────────────────────────────
// Patch the resolved _rtld_global_ro to set dl_pagesize,
// dl_tls_static_size, and dl_tls_static_align. These fields are
// normally set by ld-linux during startup, but since we use our own
// dynamic linker, they remain 0. Without them:
//   - __getpagesize asserts GLRO(dl_pagesize) != 0
//   - _dl_allocate_tls_storage allocates 0 bytes → assert(size != 0)
//
// We resolve _rtld_global_ro via the global symbol table (which points
// to ld-linux's data section after relocations). Then we write the
// correct values at known offsets (determined from glibc 2.36 disassembly).
//
// We ONLY patch these specific fields — the rest of the struct retains
// whatever values ld-linux's .data section has (mostly zeros, which
// glibc handles gracefully via NULL checks).
// ── detect_tls_field_offsets_ ──────────────────────────────────────────
// Dynamically detect the offsets of dl_tls_static_size and
// dl_tls_static_align within struct rtld_global_ro by disassembling
// __libc_early_init in libc.so.6.
//
// Background: the offsets of these fields CHANGED between glibc versions:
//   glibc 2.36 (Arm GNU 13.2):  size @ +0x1D0, align @ +0x1D8
//   glibc 2.40 (Arm GNU 14.2):  size @ +0x1D8, align @ +0x1E0
//   glibc 2.42 (Arm GNU 15.2):  size @ +0x98,  align @ +0xA0
//
// Hardcoding offsets breaks when the toolchain is upgraded. Instead, we
// disassemble __libc_early_init to find the actual LDP offset it uses to
// load these two consecutive fields.
//
// The function pattern varies by glibc version:
//   glibc 2.36/2.40: adrp x1,...; ldr x1,[x1,#...]; ldp x?,x?,[x1,#off]
//     → base register is x1, offset range 0x100-0x300
//   glibc 2.42:      adrp x0,...; ldr x0,[x0,#...]; ldr x2,[x0,#0x18];
//                     ldp x1,x0,[x0,#off]
//     → base register is x0, offset can be as low as 0x98
//
// We scan the first ~48 instructions of __libc_early_init for an LDP
// that:
//   1. Uses a base register that was recently loaded from an adrp+ldr
//      pair (we track which register holds the _rtld_global_ro pointer).
//   2. Has an offset in the range 0x18..0x400 (covers all known glibc
//      versions, excludes prologue LDP x29,x30 at offset 0).
//
// Returns true on success, filling out_size_off and out_align_off.
// Returns false if detection fails (caller falls back to known offsets).
bool DynamicLinker::detect_dlopen_hook_offset_(uint32_t& out_hook_off) {
    out_hook_off = 0;
    // Prefer dlopen@@GLIBC_2.34; fall back to __libc_dlopen_mode.
    const char* fn_name = "dlopen";
    uint64_t fn = resolve_symbol("dlopen");
    if (fn == 0) {
        fn_name = "__libc_dlopen_mode";
        fn = resolve_symbol("__libc_dlopen_mode");
    }
    if (fn == 0) return false;
    uint8_t code[96];
    try {
        mem_.read(fn, code, sizeof(code));
    } catch (...) {
        return false;
    }
    // Follow the relocated GOT load, rather than assuming one ABI offset.
    // AArch64 glibc 2.39 uses +672; other builds use +368 or +376.
    // Only accept a field load whose base is the actual _rtld_global_ro
    // address, so an unrelated ADRP/LDR pair cannot select a wrong field.
    const uint64_t rtld_ro = resolve_symbol("_rtld_global_ro");
    if (rtld_ro == 0) return false;
    uint64_t values[32] = {};
    bool known[32] = {};
    for (size_t i = 0; i + 4 <= sizeof(code); i += 4) {
        uint32_t insn;
        memcpy(&insn, code + i, 4);
        const uint32_t rt = insn & 0x1F;
        if ((insn & 0x9F000000) == 0x90000000) {
            uint64_t imm = ((insn >> 5) & 0x7FFFFu) << 2;
            imm |= (insn >> 29) & 3u;
            const int64_t signed_imm = (imm & (1u << 20))
                ? static_cast<int64_t>(imm) - (1ll << 21)
                : static_cast<int64_t>(imm);
            values[rt] = ((fn + i) & ~uint64_t{4095}) + signed_imm * 4096;
            known[rt] = rt != 31;
            continue;
        }
        if ((insn & 0xFFC00000) != 0xF9400000) continue;
        const uint32_t rn = (insn >> 5) & 0x1F;
        const uint32_t off = ((insn >> 10) & 0xFFF) * 8;
        if (known[rn] && values[rn] == rtld_ro &&
            off >= 0x100 && off <= 0x400) {
            out_hook_off = off;
            if (dynlink_trace_enabled()) {
                fprintf(stderr, "[dynlink] detected dlopen hook offset "
                        "+0x%x via %s @0x%llx+%zu\n",
                        off, fn_name,
                        static_cast<unsigned long long>(fn), i);
            }
            return true;
        }
        const bool base_known = known[rn];
        const uint64_t address = values[rn] + off;
        known[rt] = false;
        if (base_known && rt != 31) {
            try {
                values[rt] = mem_.load<uint64_t>(address);
                known[rt] = true;
            } catch (...) { /* Not a readable GOT entry. */ }
        }
    }
    return false;
}
bool DynamicLinker::detect_tls_field_offsets_(uint32_t& out_size_off,
                                               uint32_t& out_align_off) {
    out_size_off = 0;
    out_align_off = 0;
    uint64_t early_init = resolve_symbol("__libc_early_init");
    if (early_init == 0) return false;
    // Read up to 192 bytes (48 instructions) of __libc_early_init.
    // glibc 2.42's version is longer (includes __getrlimit + rlimit
    // adjustment before the TLS LDP), so we scan more than the old 32.
    uint8_t code[192];
    try {
        mem_.read(early_init, code, sizeof(code));
    } catch (...) {
        return false;
    }
    // Track which register holds the _rtld_global_ro pointer.
    // The pattern is: adrp xN, <page>; ldr xN, [xN, #<offset>]
    // After the ldr, xN holds &_rtld_global_ro.
    // We accept any register as the base (not just x1) because glibc
    // 2.42 uses x0 instead of x1.
    uint32_t rtld_ro_reg = 0xFFFFFFFF;  // invalid sentinel
    bool rtld_ro_reg_valid = false;
    for (size_t i = 0; i + 4 <= sizeof(code); i += 4) {
        uint32_t insn;
        memcpy(&insn, code + i, 4);
        // Detect ADRP: 1 immlo 10000 immhi Rd
        // mask 0x9F000000, value 0x90000000
        if ((insn & 0x9F000000) == 0x90000000) {
            uint32_t rd = insn & 0x1F;
            // Next instruction might be LDR xRd, [xRd, #imm]
            // We'll check on the next iteration.
            // For now, just remember this register had an ADRP.
            // (We don't track the ADRP target page — we just note
            //  that xRd is a candidate for the rtld_global_ro pointer.)
            rtld_ro_reg = rd;
            rtld_ro_reg_valid = false;  // not yet — need the LDR
            continue;
        }
        // Detect LDR (64-bit GPR, unsigned offset): 11 111 0 01 01 imm12 Rn Rt
        // mask 0xFFC00000, value 0xF9400000
        if ((insn & 0xFFC00000) == 0xF9400000) {
            uint32_t rn = (insn >> 5) & 0x1F;
            uint32_t rt = insn & 0x1F;
            // Only follow the chain when rt == rn (i.e., ldr xN, [xN, #imm]).
            // This is the pattern: adrp xN, <page>; ldr xN, [xN, #imm] →
            // xN now holds the value at that address (the rtld_global_ro
            // pointer). If rt != rn (e.g., ldr x2, [x0, #24] to load
            // dl_pagesize), x2 does NOT hold the rtld_global_ro pointer.
            if (rtld_ro_reg != 0xFFFFFFFF && rn == rtld_ro_reg && rt == rn) {
                rtld_ro_reg = rt;
                rtld_ro_reg_valid = true;
            }
            continue;
        }
        // Detect LDP (64-bit GPR, unsigned offset):
        //   10 1010 0101 0 imm7 Rt2 Rn Rt
        //   mask 0xFFC00000, value 0xA9400000
        if ((insn & 0xFFC00000) != 0xA9400000) continue;
        uint32_t rn = (insn >> 5) & 0x1F;
        // The base must be the register holding _rtld_global_ro.
        if (!rtld_ro_reg_valid || rn != rtld_ro_reg) continue;
        // imm7 is at bits 21-15 (7 bits, unsigned for this variant).
        uint32_t imm7 = (insn >> 15) & 0x7F;
        uint32_t offset = imm7 * 8;  // scaled by 8 for 64-bit
        // Sanity: offset must be in a reasonable range (0x18..0x400)
        // for rtld_global_ro's fields. This excludes LDP x29,x30
        // in the prologue (offset 0).
        // 0x18 = dl_pagesize offset (stable across versions).
        // The TLS fields are at a higher offset, but we accept any
        // non-zero offset here because the LDP we want is the ONLY
        // LDP from the rtld_global_ro register (besides the pagesize
        // LDR which uses a different instruction).
        if (offset < 0x18 || offset > 0x400) continue;
        // Heuristic: the LDP we want loads (dl_tls_static_size,
        // dl_tls_static_align). In glibc 2.42, the LDP at offset 0x98
        // loads (size, align). In glibc 2.40, the LDP at 0x1D8 loads
        // (size, align). We accept any LDP from the rtld_global_ro
        // register as the TLS field pair.
        out_size_off = offset;
        out_align_off = offset + 8;
        if (dynlink_trace_enabled()) {
            fprintf(stderr, "[dynlink] detected TLS field offsets via "
                    "__libc_early_init @0x%llx+%zu: size@+0x%x, align@+0x%x "
                    "(imm7=%u, base=x%u)\n",
                    static_cast<unsigned long long>(early_init), i,
                    out_size_off, out_align_off, imm7, rtld_ro_reg);
        }
        return true;
    }
    return false;
}
void DynamicLinker::patch_rtld_global_ro_() {
    if (pending_tls_static_size_ == 0) return;  // no TLS → nothing to patch
    uint64_t rtld_ro = resolve_symbol("_rtld_global_ro");
    if (rtld_ro == 0) {
        // _rtld_global_ro not found — might be a musl binary (no _rtld_global_ro).
        return;
    }
    // ── Dynamic offset detection ────────────────────────────────────
    // The offsets of dl_tls_static_size and dl_tls_static_align within
    // struct rtld_global_ro vary by glibc version:
    //   glibc 2.36 (Arm GNU 13.2): size @ +0x1D0, align @ +0x1D8
    //   glibc 2.40 (Arm GNU 14.2): size @ +0x1D8, align @ +0x1E0
    // Hardcoding breaks when the toolchain is upgraded. We disassemble
    // __libc_early_init to find the actual LDP offset it uses to load
    // these two consecutive fields. If detection fails, we fall back to
    // spraying ALL known offsets (safe because the fields are consecutive
    // size_t values and spraying writes the same value to adjacent slots).
    uint32_t size_off = 0, align_off = 0;
    bool detected = detect_tls_field_offsets_(size_off, align_off);
    // Known offset pairs (glibc version → (size_off, align_off)).
    // Used as fallback if dynamic detection fails.
    struct KnownOffset { uint32_t size; uint32_t align; const char* ver; };
    constexpr KnownOffset known_offsets[] = {
        {0x98,  0xA0,  "glibc 2.42 (Arm GNU 15.2)"},  // check newest first
        {0x1D8, 0x1E0, "glibc 2.40 (Arm GNU 14.2)"},
        {0x1D0, 0x1D8, "glibc 2.36 (Arm GNU 13.2)"},
    };
    // Pagesize is at offset 0x18 in all known glibc versions.
    constexpr uint32_t PAGESIZE_OFF = 0x18;
    try {
        // ── Patch dl_pagesize (offset 0x18, stable across versions) ──
        // BUGFIX: write unconditionally. ld-linux's .data.rel.ro carries
        // a hardcoded initializer _dl_pagesize = EXEC_PAGESIZE = 0x10000
        // (64 KiB) for AArch64 builds. On a real boot, ld.so's
        // _dl_aux_init unconditionally overrides that from AT_PAGESZ
        // (=4096, the size the kernel/emulator actually uses). Since the
        // native dynlink never runs ld.so's startup, the stale 0x10000
        // would otherwise survive: glibc malloc rounds mmaps to 64 KiB
        // and munmap_chunk demands 64 KiB-aligned blocks, but the
        // emulator's mmap_alloc only aligns to 4096 → "munmap_chunk():
        // invalid pointer". Write 4096 (the value AT_PAGESZ reports).
        mem_.store<uint64_t>(rtld_ro + PAGESIZE_OFF, 4096);
        // ── Patch dl_tls_static_size and dl_tls_static_align ─────────
        // Strategy: if dynamic detection succeeded, patch exactly those
        // two offsets. Otherwise, "spray" — write the size and align
        // values to ALL known offset pairs. Spraying is safe because:
        //   1. The fields are consecutive size_t values in the struct.
        //   2. Writing a valid size to an adjacent field that happens
        //      to be dl_tls_static_used or dl_tls_static_surplus is
        //      harmless (glibc adds them to the size, and a slightly
        //      larger size just means a bit more TLS surplus).
        //   3. We only write if the current value is 0, so we never
        //      clobber a field that ld-linux already initialized.
        if (detected) {
            // Precise patching — write only the detected offsets.
            uint64_t cur_size = mem_.load<uint64_t>(rtld_ro + size_off);
            if (cur_size == 0) {
                mem_.store<uint64_t>(rtld_ro + size_off,
                                     pending_tls_static_size_);
            }
            uint64_t cur_align = mem_.load<uint64_t>(rtld_ro + align_off);
            if (cur_align == 0) {
                mem_.store<uint64_t>(rtld_ro + align_off,
                                     pending_tls_static_align_);
            }
        } else {
            // Fallback: spray all known offset pairs.
            if (dynlink_trace_enabled()) {
                fprintf(stderr, "[dynlink] TLS offset detection failed; "
                        "spraying all known offsets\n");
            }
            for (const auto& ko : known_offsets) {
                uint64_t cur_size = mem_.load<uint64_t>(rtld_ro + ko.size);
                if (cur_size == 0) {
                    mem_.store<uint64_t>(rtld_ro + ko.size,
                                         pending_tls_static_size_);
                }
                uint64_t cur_align = mem_.load<uint64_t>(rtld_ro + ko.align);
                if (cur_align == 0) {
                    mem_.store<uint64_t>(rtld_ro + ko.align,
                                         pending_tls_static_align_);
                }
            }
        }
        if (dynlink_trace_enabled()) {
            fprintf(stderr, "[dynlink] patched _rtld_global_ro @0x%llx: "
                    "dl_pagesize=4096, dl_tls_static_size=%llu, "
                    "dl_tls_static_align=%llu (%s)\n",
                    static_cast<unsigned long long>(rtld_ro),
                    static_cast<unsigned long long>(pending_tls_static_size_),
                    static_cast<unsigned long long>(pending_tls_static_align_),
                    detected ? "dynamic detection"
                             : "spray fallback");
        }
    } catch (...) {
        // Reading/writing _rtld_global_ro failed — the struct might
        // not be mapped at the expected address. This is non-fatal;
        // glibc will hit the assertion later (visible failure).
    }
    // dlopen / dlfcn hook pointer inside _rtld_global_ro.
    // glibc's dlopen@@GLIBC_2.34 and __libc_dlopen_mode do:
    //   ldr xN, [rtld_global_ro, #hook_off]  → hook struct*
    //   ldr xM, [xN] / [xN, #72]             → _dl_open
    // The hook field offset moved across glibc versions:
    //   glibc 2.39 (Bootlin/Ubuntu): +672 (0x2a0)
    //   some Arm GNU builds: +368 (0x170) or +376 (0x178)
    // Hardcoding 368 made dlopen fall into the no-hook path on 2.43,
    // which calls the real ld-linux _dl_open and ends at pc=0.
    if (dlopen_hook_ptr_ != 0) {
        try {
            uint32_t hook_off = 0;
            bool hook_detected = detect_dlopen_hook_offset_(hook_off);
            // Known offsets newest-first; spray only empty slots so we
            // never clobber a non-NULL field that belongs to another
            // member of rtld_global_ro.
            constexpr uint32_t known_hook_offs[] = {376, 368};
            if (hook_detected) {
                mem_.store<uint64_t>(rtld_ro + hook_off, dlopen_hook_ptr_);
                if (dynlink_trace_enabled()) {
                    fprintf(stderr, "[dynlink] patched _rtld_global_ro + %u "
                            "= 0x%llx (dlopen hook, detected)\n",
                            hook_off,
                            static_cast<unsigned long long>(dlopen_hook_ptr_));
                }
            } else {
                for (uint32_t off : known_hook_offs) {
                    uint64_t cur = 0;
                    try { cur = mem_.load<uint64_t>(rtld_ro + off); }
                    catch (...) { continue; }
                    if (cur != 0) continue;
                    mem_.store<uint64_t>(rtld_ro + off, dlopen_hook_ptr_);
                    if (dynlink_trace_enabled()) {
                        fprintf(stderr, "[dynlink] patched _rtld_global_ro + %u "
                                "= 0x%llx (dlopen hook, spray)\n",
                                off,
                                static_cast<unsigned long long>(dlopen_hook_ptr_));
                    }
                }
            }
        } catch (...) {}
    }
}
// ── patch_rtld_funnel_ ────────────────────────────────────────────────
// Funnelled glibc (2.42+): glibc was built with function-visibility
// tuning that puts the real _dl_find_object implementation in ld-linux
// and exports a 4-instruction trampoline from libc:
//   adrp x2, <got_page>
//   ldr  x2, [x2, #<got_off>]        ; x2 = &_rtld_global_ro
//   ldr  x16, [x2, #<slot_off>]      ; x16 = GLRO(dl_find_object)
//   br   x16
// The target slot in _rtld_global_ro is normally written by ld-linux's
// _dl_start_final:
//   add x2, x?, #0xc98               ; x2 = &_rtld_global_ro
//   adrp x1, ...; add x1, x1, #imm   ; &_dl_readonly_area
//   adrp x0, ...; add x0, x0, #imm   ; &_dl_find_object
//   stp  x0, x1, [x2, #<slot_off>]
// Since the native dynlink never runs ld-linux's _dl_start, the slot
// stays 0 and the trampoline does `br 0` → SIGSEGV at pc=0. We detect
// the slot offset from the libc trampoline, locate the matching STP in
// ld-linux's _dl_start, and write the real function addresses so the
// trampoline forwards correctly. No-op for non-funnelled glibc / musl.
void DynamicLinker::patch_rtld_funnel_() {
    // A funnelled libc exports _dl_find_object as a trampoline:
    //   adrp x2, <got_page>
    //   ldr  x2, [x2, #<got_off>]        ; x2 = &_rtld_global_ro
    //   ldr  x16, [x2, #<slot_off>]      ; x16 = GLRO(dl_find_object)
    //   br   x16
    // Extract the _rtld_global_ro slot offset. resolve_symbol normally
    // returns the trampoline, but the _dl_find_object FORCE override in
    // register_ld_linux_shim_() points it at our stub — in that case
    // scan libc's mapped text for the `ldr xN, [xM, #slot]; br xN` tail.
    uint64_t tramp = resolve_symbol("_dl_find_object");
    uint32_t slot_off = 0;
    if (tramp != 0 && tramp != dl_find_object_stub_) {
        uint8_t code[32];
        try {
            mem_.read(tramp, code, sizeof(code));
        } catch (...) {
            return;
        }
        for (size_t i = 0; i + 8 <= sizeof(code); i += 4) {
            uint32_t insn, nxt;
            memcpy(&insn, code + i, 4);
            memcpy(&nxt, code + i + 4, 4);
            // LDR (64-bit, unsigned offset) then BR (br xN: mask/value
            // 0xFFFFFC1F/0xD61F0000 — ignores the register bits).
            if ((insn & 0xFFC00000) == 0xF9400000 &&
                (nxt & 0xFFFFFC1F) == 0xD61F0000) {
                slot_off = ((insn >> 10) & 0xFFF) * 8;
                break;
            }
        }
    } else {
        // The shim override hides libc's trampoline from resolve_symbol.
        // Find the slot by scanning libc's own .text for the trampoline
        // tail; the LDR/BR pair reads x16 out of _rtld_global_ro.
        const LoadedObject* libc = find_object_by_addr(resolve_symbol("malloc"));
        if (libc != nullptr) {
            const uint64_t libc_text_end =
                libc->base_addr + std::min<uint64_t>(libc->map_size, 0x40000);
            for (uint64_t p = libc->base_addr + 0x1000; p + 8 <= libc_text_end; p += 4) {
                uint32_t insn, nxt;
                try {
                    insn = mem_.load<uint32_t>(p);
                    nxt  = mem_.load<uint32_t>(p + 4);
                } catch (...) {
                    break;
                }
                if ((insn & 0xFFC00000) == 0xF9400000 &&
                    (nxt & 0xFFFFFC1F) == 0xD61F0000 &&
                    (insn & 0x1F) == (nxt & 0x1F)) {
                    uint32_t cand = ((insn >> 10) & 0xFFF) * 8;
                    if (cand >= 0x100 && cand < 0x400) {
                        slot_off = cand;
                        break;
                    }
                }
            }
        }
    }
    if (slot_off == 0 || slot_off >= 0x400) return;
    uint64_t rtld_ro = resolve_symbol("_rtld_global_ro");
    if (rtld_ro == 0) return;
    const LoadedObject* ld = find_object_by_addr(rtld_ro);
    if (ld == nullptr || ld->base_addr == 0) return;
    const uint64_t ld_base = ld->base_addr;
    // Scan ld-linux's mapped image for an STP/STR storing into
    // _rtld_global_ro at slot_off. Restrict to the .text mapping.
    const uint64_t text_end = ld_base + std::min<uint64_t>(ld->map_size, 0x28000);
    uint64_t dl_find_object_va = 0;  // file VA of the real implementation
    uint64_t dl_readonly_area_va = 0;
    for (uint64_t p = ld_base + 0x10; p + 8 <= text_end; p += 4) {
        uint32_t insn;
        try {
            insn = mem_.load<uint32_t>(p);
        } catch (...) {
            break;
        }
        uint32_t off = 0;
        uint32_t rt = 0xFFFFFFFF, rt2 = 0xFFFFFFFF;
        if ((insn & 0xFFC00000) == 0xA9000000) {
            // STP (64-bit, unsigned offset, no writeback): imm7 scaled
            // by 8. Rt = dst[0], Rt2 = dst[1].
            off = ((insn >> 15) & 0x7F) * 8;
            rt = insn & 0x1F;
            rt2 = (insn >> 10) & 0x1F;
        } else if ((insn & 0xFFC00000) == 0xF9400000) {
            // STR (64-bit, unsigned offset).
            off = ((insn >> 10) & 0xFFF) * 8;
            rt = insn & 0x1F;
        }
        if (off != slot_off) continue;
        // Resolve the source register(s): the compiler computes the
        // function address with `adrp xR, <page>; add xR, xR, #imm`
        // within the instructions just before the store. Walk backwards
        // up to 16 instructions tracking the last adrp per register.
        auto resolve_src = [&](uint32_t reg) -> uint64_t {
            uint64_t add_page = 0, add_imm = 0;
            bool have_add = false;
            // Walk BACKWARDS (closest-to-store first) so we see the
            // `add xR, xR, #imm` before its preceding `adrp xR, <page>`.
            uint64_t low = (p > 64 ? p - 64 : ld_base);
            for (uint64_t q = p; q > low; ) {
                q -= 4;
                uint32_t w;
                try { w = mem_.load<uint32_t>(q); }
                catch (...) { break; }
                if ((w & 0x7F000000) == 0x11000000) {
                    uint32_t rd = w & 0x1F, rn = (w >> 5) & 0x1F;
                    if (rd == reg && rn == reg) {
                        add_imm = (w >> 10) & 0xFFF;
                        have_add = true;
                    }
                    continue;
                }
                if ((w & 0x9F000000) == 0x90000000) {
                    uint32_t rd = w & 0x1F;
                    if (rd == reg && have_add) {
                        // ADRP: pc-relative page of the CURRENT instr.
                        int64_t imm = ((w >> 5) & 0x7FFFF) << 2;
                        imm |= (w >> 29) & 0x3;
                        if (imm & (1LL << 20)) imm -= (1LL << 21);
                        add_page = (q & ~0xFFFULL) + (imm << 12);
                        break;
                    }
                }
            }
            if (add_page == 0 || !have_add) return 0;
            return add_page + add_imm;
        };
        uint64_t v0 = resolve_src(rt);
        uint64_t v1 = (rt2 != 0xFFFFFFFF) ? resolve_src(rt2) : 0;
        if (v0 == 0) continue;
        dl_find_object_va = v0;
        dl_readonly_area_va = v1;
        if (dynlink_trace_enabled()) {
            fprintf(stderr, "[dynlink] funnel STP @0x%llx: slot_off=0x%x "
                    "-> _dl_find_object=0x%llx, _dl_readonly_area=0x%llx\n",
                    static_cast<unsigned long long>(p), slot_off,
                    static_cast<unsigned long long>(v0),
                    static_cast<unsigned long long>(v1));
        }
        break;
    }
    if (dl_find_object_va == 0 && dl_find_object_stub_ == 0) return;
    // Prefer our native shim stub: it answers _dl_find_object from host
    // LoadedObject state (real eh_frame_hdr etc.) via syscall 0x1008,
    // which libgcc's _Unwind_Find_FDE needs for C++ exceptions. Fall
    // back to ld-linux's real implementation only when no shim exists.
    const uint64_t fn_addr = dl_find_object_stub_ != 0
                                 ? dl_find_object_stub_
                                 : dl_find_object_va;
    if (dl_find_object_stub_ == 0) {
        // resolve_src yields ABSOLUTE guest addresses. Sanity: the
        // resolved function must lie inside ld-linux's mapped image
        // before writing.
        const uint64_t map_end = ld_base + std::min<uint64_t>(ld->map_size, 0x28000);
        if (dl_find_object_va < ld_base + 0x1000 || dl_find_object_va >= map_end)
            return;
        if (dl_readonly_area_va != 0 &&
            (dl_readonly_area_va < ld_base + 0x1000 || dl_readonly_area_va >= map_end))
            dl_readonly_area_va = 0;
    }
    try {
        mem_.store<uint64_t>(rtld_ro + slot_off, fn_addr);
        if (dl_readonly_area_va != 0)
            mem_.store<uint64_t>(rtld_ro + slot_off + 8, dl_readonly_area_va);
        if (dynlink_trace_enabled()) {
            fprintf(stderr, "[dynlink] patched _rtld_global_ro + 0x%x = 0x%llx "
                    "(_dl_find_object), +0x%x = 0x%llx (_dl_readonly_area)\n",
                    slot_off,
                    static_cast<unsigned long long>(fn_addr),
                    slot_off + 8,
                    static_cast<unsigned long long>(dl_readonly_area_va));
        }
    } catch (...) {}
}
// ── init_nptl_stack_lists_ ────────────────────────────────────────────
// Initialize the NPTL stack-cache list heads in _rtld_global so that
// glibc's pthread_create -> allocate_stack does not spin forever.
//
// Background: glibc (2.34+, NPTL merged into libc) keeps three circular
// doubly-linked lists in `struct rtld_global` to manage thread stacks:
//
//   _dl_stack_used   — threads currently using a stack
//   _dl_stack_user   — threads which need a stack
//   _dl_stack_cache  — cache of free, reusable stacks
//
// Each is a `list_t` (= `struct list_head { list_t *next, *prev; }`,
// 16 bytes). An EMPTY list must have head->next == head->prev == &head
// (the INIT_LIST_HEAD macro). `allocate_stack` walks `_dl_stack_cache`
// via `list_for_each(entry, &GL(dl_stack_cache))`, which expands to
// `for (entry = head->next; entry != head; entry = entry->next)`. If
// the list is properly empty (head->next == head), the body is skipped
// and a fresh stack is allocated.
//
// The bug: ld-linux's `__pthread_initialize_minimal_internal` calls
// INIT_LIST_HEAD on all three heads during startup, but we use our own
// dynamic linker (not the guest ld-linux), so those heads stay zeroed
// (they live in ld-linux's .bss, mapped read-write). With head->next ==
// NULL, the `list_for_each` loop dereferences NULL -> bifrost-emu
// returns 0 for unmapped reads (instead of faulting) -> entry stays 0
// forever -> infinite CPU spin (no syscall, no futex). This matches the
// observed hang: test_dyn_pthread_min prints "start", then spins in
// libc.so.6 + 0x8102c (inside pthread_create) without making any
// syscall.
//
// The fix: resolve `_rtld_global` (the read-write rtld global, NOT
// `_rtld_global_ro`) and write self-referential pointers into the three
// list heads.
//
// ── DYNAMIC OFFSET DETECTION ───────────────────────
// The three list heads live at version-dependent offsets within
// `struct rtld_global`. Rather than hardcode offsets for one glibc
// version, we discover them at runtime from the libthread_db
// descriptors that glibc exports in libc.so.6 .rodata:
//
//   _thread_db_rtld_global__dl_stack_used  (12-byte descriptor)
//   _thread_db_rtld_global__dl_stack_user  (12-byte descriptor)
//
// Each descriptor is `uint32_t[3] = { size_in_bits, element_count,
// byte_offset }` (nptl_db/db_info.c); the field offset within
// `struct rtld_global` is the 32-bit value at byte +8. The descriptors
// are .symtab-only locals, found via resolve_thread_db_symbol_().
//
// The three list heads are CONTIGUOUS 16-byte `list_t`s:
//   stack_used  @ desc_used.offset
//   stack_user  @ desc_used.offset + 16   (= stack_used + sizeof(list_t))
//   stack_cache @ desc_used.offset + 32
// We cross-check: desc_user.offset MUST equal desc_used.offset + 16;
// if it doesn't, the layout assumption is wrong for this glibc and we
// fall back to NOT initializing (safer than writing at wrong offsets —
// glibc will spin, a visible failure, rather than corrupt memory).
//
// Fallback: if the descriptors aren't found (stripped libc, or a libc
// without NPTL), we fall back to the glibc 2.36 / Arm GNU 13.2 offsets
// (0x1158 / 0x1168 / 0x1178). These are correct for the toolchain we
// ship tests against; the dynamic path covers other versions.
//
// This is glibc-specific (musl has no _rtld_global); if the symbol is
// not found (musl, or static binary), this is a no-op. We also guard
// against double-init: if a head already points to itself (or to a
// non-zero value — meaning ld-linux init already ran), we leave it
// alone so we never corrupt a populated list.
// ── libthread_db descriptors ──────────────────────────────────────────
// glibc's nptl_db/db_info.c emits `const uint32_t _thread_db_sizeof_<T>`
// and, per field, `const uint32_t _thread_db_<T>_<field>[3] =
// { size_in_bits, element_count, byte_offset }`. They are local symbols
// (.symtab only), so stripped libcs simply yield nothing here.
void DynamicLinker::collect_thread_db_symbols_(const std::vector<uint8_t>& data,
                                               uint64_t base, LoadedObject& obj) {
    constexpr uint32_t SHT_SYMTAB = 2;
    constexpr size_t SHDR_SIZE = 64;
    uint64_t shoff = 0;
    uint16_t shentsize = 0, shnum = 0;
    memcpy(&shoff,     data.data() + 40, 8);
    memcpy(&shentsize, data.data() + 58, 2);
    memcpy(&shnum,     data.data() + 60, 2);
    if (shoff == 0 || shentsize < SHDR_SIZE || shoff > data.size() ||
        static_cast<uint64_t>(shnum) * shentsize > data.size() - shoff)
        return;
    auto section = [&](uint32_t index, uint32_t& type, uint64_t& offset,
                       uint64_t& size, uint32_t& link) {
        const uint8_t* s = data.data() + shoff + static_cast<uint64_t>(index) * shentsize;
        memcpy(&type,   s + 4,  4);
        memcpy(&offset, s + 24, 8);
        memcpy(&size,   s + 32, 8);
        memcpy(&link,   s + 40, 4);
    };
    for (uint32_t i = 0; i < shnum; ++i) {
        uint32_t type, link, str_type, str_link;
        uint64_t off, size, str_off, str_size;
        section(i, type, off, size, link);
        if (type != SHT_SYMTAB || link >= shnum) continue;
        section(link, str_type, str_off, str_size, str_link);
        if (off > data.size() || size > data.size() - off ||
            str_off > data.size() || str_size > data.size() - str_off)
            return;
        const char* strtab = reinterpret_cast<const char*>(data.data() + str_off);
        static const char kPrefix[] = "_thread_db_";
        for (uint64_t pos = off; pos + sizeof(Elf64_Sym) <= off + size;
             pos += sizeof(Elf64_Sym)) {
            Elf64_Sym sym;
            memcpy(&sym, data.data() + pos, sizeof(sym));
            if (sym.st_shndx == SHN_UNDEF_ || sym.st_name >= str_size) continue;
            const char* name = strtab + sym.st_name;
            const size_t max_len = str_size - sym.st_name;
            if (strnlen(name, max_len) == max_len ||
                strncmp(name, kPrefix, sizeof(kPrefix) - 1) != 0)
                continue;
            obj.thread_db_syms.emplace(name, base + sym.st_value);
        }
        return;
    }
}
uint64_t DynamicLinker::resolve_thread_db_symbol_(const std::string& name) const {
    if (uint64_t addr = resolve_symbol(name)) return addr;
    for (const auto& obj : objects_) {
        auto it = obj.thread_db_syms.find(name);
        if (it != obj.thread_db_syms.end()) return it->second;
    }
    return 0;
}
// glibc's __tls_init_tp (ld.so) runs `pd->tid = set_tid_address(&pd->tid)`
// and `list_add(&pd->list, &GL(dl_stack_user))` for the initial thread.
// Our native linker skips ld.so's startup, so pd->tid stayed 0: every
// rwlock and error-checking/recursive mutex on the main thread compares
// the owner against it and failed with EDEADLK. OpenSSL 3 takes an
// rwlock while creating its default library context, so no provider
// could load. struct pthread sits directly below TP (THREAD_SELF =
// tp - 1), sized by _thread_db_sizeof_pthread. specific[0] and
// user_stack have no descriptors and are left to glibc's fallbacks.
void DynamicLinker::init_main_thread_descriptor_(const CPU& cpu) {
    if (is_musl_) return;
    const uint64_t size_sym = resolve_thread_db_symbol_("_thread_db_sizeof_pthread");
    const uint64_t tid_desc = resolve_thread_db_symbol_("_thread_db_pthread_tid");
    if (size_sym == 0 || tid_desc == 0) return;
    try {
        const uint32_t pthread_size = mem_.load<uint32_t>(size_sym);
        const uint32_t tid_bits = mem_.load<uint32_t>(tid_desc);
        const uint32_t tid_off = mem_.load<uint32_t>(tid_desc + 8);
        const uint64_t tp = thread_pointer();
        if (pthread_size == 0 || pthread_size > TLS_PRE_TCB_SIZE ||
            tid_bits != 32 || tid_off + 4u > pthread_size || tp < pthread_size)
            return;
        const uint64_t pd = tp - pthread_size;
        if (mem_.load<int32_t>(pd + tid_off) == 0)
            mem_.store<int32_t>(pd + tid_off, cpu.tid);
        // list_add(&pd->list, &dl_stack_user); heads are initialized above.
        const uint64_t list_desc = resolve_thread_db_symbol_("_thread_db_pthread_list");
        const uint64_t user_desc =
            resolve_thread_db_symbol_("_thread_db_rtld_global__dl_stack_user");
        const uint64_t rtld = resolve_symbol("_rtld_global");
        if (list_desc == 0 || user_desc == 0 || rtld == 0) return;
        const uint32_t list_off = mem_.load<uint32_t>(list_desc + 8);
        if (list_off + 16u > pthread_size) return;
        const uint64_t node = pd + list_off;
        const uint64_t head = rtld + mem_.load<uint32_t>(user_desc + 8);
        const uint64_t first = mem_.load<uint64_t>(head);
        if (mem_.load<uint64_t>(node) != 0 || first == 0) return;
        mem_.store<uint64_t>(node, first);       // node->next
        mem_.store<uint64_t>(node + 8, head);    // node->prev
        mem_.store<uint64_t>(first + 8, node);   // first->prev
        mem_.store<uint64_t>(head, node);        // head->next
        if (dynlink_trace_enabled())
            fprintf(stderr, "[dynlink] main struct pthread @0x%llx: tid=%d "
                    "(sizeof %u, tid@+%u, list@+%u)\n",
                    static_cast<unsigned long long>(pd), cpu.tid,
                    pthread_size, tid_off, list_off);
    } catch (...) {
        // Unreadable descriptor or TLS block: leave glibc's state alone.
    }
}
void DynamicLinker::init_nptl_stack_lists_() {
    uint64_t rtld = resolve_symbol("_rtld_global");
    if (rtld == 0) {
        // musl or static binary — no _rtld_global. Nothing to do.
        return;
    }
    // ── Discover the stack_used / stack_user offsets dynamically ──
    // Default to the glibc 2.36 / Arm GNU 13.2 offsets (our shipped
    // toolchain). Try to override with the libthread_db descriptor
    // values for portability across glibc versions.
    uint64_t off_used  = 0x1158;
    uint64_t off_user  = 0x1168;
    uint64_t off_cache = 0x1178;
    bool dynamic = false;
    uint64_t desc_used = resolve_thread_db_symbol_(
        "_thread_db_rtld_global__dl_stack_used");
    uint64_t desc_user = resolve_thread_db_symbol_(
        "_thread_db_rtld_global__dl_stack_user");
    if (desc_used != 0 && desc_user != 0) {
        try {
            // The field offset is a 32-bit LE value at descriptor +8.
            uint32_t u = mem_.load<uint32_t>(desc_used + 8);
            uint32_t v = mem_.load<uint32_t>(desc_user + 8);
            // Cross-check the contiguity invariant:
            //   stack_user == stack_used + 16 (one list_t).
            // If it holds, the layout matches our assumption; adopt the
            // dynamic offsets. If not, keep the fallback (don't risk
            // writing at inconsistent offsets).
            if (v == u + 16 && u != 0) {
                off_used  = u;
                off_user  = v;
                off_cache = static_cast<uint64_t>(u) + 32;
                dynamic = true;
            }
        } catch (...) {
            // Descriptor not readable — keep the fallback offsets.
        }
    }
    struct ListHeadOff { const char* name; uint64_t off; };
    const ListHeadOff heads[] = {
        { "_dl_stack_used",  off_used  },
        { "_dl_stack_user",  off_user  },
        { "_dl_stack_cache", off_cache },
    };
    bool patched = false;
    try {
        for (const auto& h : heads) {
            uint64_t head_addr = rtld + h.off;
            uint64_t next = mem_.load<uint64_t>(head_addr);
            // Only initialize if the head is still zero (uninitialized).
            // If next is non-zero, either ld-linux already initialized it
            // (self-pointer) or a previous pthread_create populated the
            // list — in either case, leave it alone.
            if (next == 0) {
                // INIT_LIST_HEAD: head->next = head->prev = &head.
                mem_.store<uint64_t>(head_addr,     head_addr);  // next
                mem_.store<uint64_t>(head_addr + 8, head_addr);  // prev
                patched = true;
            }
        }
    } catch (...) {
        // _rtld_global not mapped at the expected range — non-fatal.
        // glibc will spin later (visible failure).
    }
    if (patched && dynlink_trace_enabled()) {
        fprintf(stderr, "[dynlink] initialized NPTL stack list heads in "
                "_rtld_global @0x%llx (%s offsets: "
                "stack_used@+0x%llx, stack_user@+0x%llx, "
                "stack_cache@+0x%llx)\n",
                static_cast<unsigned long long>(rtld),
                dynamic ? "dynamic" : "fallback",
                static_cast<unsigned long long>(off_used),
                static_cast<unsigned long long>(off_user),
                static_cast<unsigned long long>(off_cache));
    }
}
}  // namespace arm64emu
