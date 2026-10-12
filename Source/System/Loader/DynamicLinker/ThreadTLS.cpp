// Static and dynamic TLS: DTV management, per-thread blocks, TLS descriptors.
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
// ── Per-thread DTV and dynamic TLS ────────────────────────────────────
void DynamicLinker::track_tls_allocation_(uint64_t tcb, uint64_t addr,
                                          uint64_t size,
                                          uint64_t module_id) {
    if (tcb && addr && size)
        tls_thread_allocations_[tcb].push_back({addr, size, module_id});
}
void DynamicLinker::release_tls_allocation_(Memory& mem, uint64_t tcb,
                                            uint64_t addr) {
    auto it = tls_thread_allocations_.find(tcb);
    if (it == tls_thread_allocations_.end()) return;
    auto& owned = it->second;
    for (auto alloc = owned.begin(); alloc != owned.end(); ++alloc) {
        if (alloc->addr != addr) continue;
        mem.untrack_allocation(alloc->addr, alloc->size);
        owned.erase(alloc);
        break;
    }
    if (owned.empty()) tls_thread_allocations_.erase(it);
}
bool DynamicLinker::ensure_thread_dtv_capacity_(Memory& mem, uint64_t tcb,
                                                 uint64_t required_module_id) {
    if (is_musl_ || tcb == 0 || static_tls_base_ == 0 ||
        dtv_storage_offset_ == 0 || dtv_capacity_ == 0) return false;
    constexpr uint64_t DTV_WORDS_PER_ENTRY = 2;
    constexpr uint64_t DTV_ENTRY_SIZE = sizeof(uint64_t) * DTV_WORDS_PER_ENTRY;
    uint64_t dtv = 0;
    try { dtv = mem.load<uint64_t>(tcb); } catch (...) { return false; }
    if (dtv == 0) {
        // A newly allocated pthread block has a zeroed TCB. Bootstrap the
        // DTV in the positive slack reserved by allocate_static_tls().
        const uint64_t raw = static_cast<uint64_t>(
            static_cast<int64_t>(tcb) + static_cast<int64_t>(dtv_storage_offset_));
        const uint64_t bytes = (dtv_capacity_ + 2) * DTV_ENTRY_SIZE;
        try {
            std::vector<uint8_t> zeros(static_cast<size_t>(bytes), 0);
            mem.write(raw, zeros.data(), zeros.size());
            mem.store<uint64_t>(raw, dtv_capacity_);
            mem.store<uint64_t>(raw + DTV_ENTRY_SIZE, tls_generation_);
            dtv = raw + DTV_ENTRY_SIZE;
            mem.store<uint64_t>(tcb, dtv);
        } catch (...) {
            return false;
        }
    }
    if (dtv < DTV_ENTRY_SIZE) return false;
    uint64_t raw = dtv - DTV_ENTRY_SIZE;
    uint64_t capacity = 0;
    try { capacity = mem.load<uint64_t>(raw); } catch (...) { return false; }
    if (capacity > 65536) return false; // reject a corrupted guest DTV header
    if (required_module_id <= capacity) return true;
    if (required_module_id > (UINT64_MAX / DTV_ENTRY_SIZE) - 2) return false;
    uint64_t new_capacity = std::max<uint64_t>(capacity ? capacity * 2 : 32,
                                                required_module_id);
    if (new_capacity > (UINT64_MAX / DTV_ENTRY_SIZE) - 2) return false;
    const uint64_t new_bytes = (new_capacity + 2) * DTV_ENTRY_SIZE;
    const uint64_t new_raw = mem.mmap_alloc(new_bytes);
    if (new_raw == 0) return false;
    try {
        std::vector<uint8_t> zeros(static_cast<size_t>(new_bytes), 0);
        mem.write(new_raw, zeros.data(), zeros.size());
        const uint64_t old_bytes = (capacity + 2) * DTV_ENTRY_SIZE;
        std::vector<uint8_t> old(old_bytes);
        mem.read(raw, old.data(), old.size());
        mem.write(new_raw, old.data(), old.size());
        mem.store<uint64_t>(new_raw, new_capacity);
        mem.store<uint64_t>(new_raw + DTV_ENTRY_SIZE, tls_generation_);
        mem.store<uint64_t>(tcb, new_raw + DTV_ENTRY_SIZE);
    } catch (...) {
        mem.untrack_allocation(new_raw, new_bytes);
        return false;
    }
    track_tls_allocation_(tcb, new_raw, new_bytes);
    const uint64_t inline_raw = static_cast<uint64_t>(
        static_cast<int64_t>(tcb) + static_cast<int64_t>(dtv_storage_offset_));
    if (raw != inline_raw) release_tls_allocation_(mem, tcb, raw);
    return true;
}
bool DynamicLinker::initialize_thread_dtv(Memory& mem, uint64_t tcb) {
    if (is_musl_) return true;
    std::lock_guard<std::recursive_mutex> lk(loader_mu_);
    if (tcb == 0) return false;
    if (static_tls_base_ == 0 || dtv_storage_offset_ == 0 ||
        dtv_capacity_ == 0) return false;
    uint64_t max_module_id = 0;
    for (const auto& obj : objects_) {
        if (obj.tls.present && obj.tls.memsz != 0)
            max_module_id = std::max(max_module_id, obj.tls_mod_id);
    }
    if (!ensure_thread_dtv_capacity_(mem, tcb, max_module_id)) return false;
    uint64_t dtv = 0;
    try { dtv = mem.load<uint64_t>(tcb); } catch (...) { return false; }
    if (dtv < sizeof(uint64_t) * 2) return false;
    const uint64_t raw = dtv - sizeof(uint64_t) * 2;
    uint64_t capacity = 0;
    try { capacity = mem.load<uint64_t>(raw); } catch (...) { return false; }
    try {
        mem.store<uint64_t>(raw, capacity);
        mem.store<uint64_t>(raw + 16, tls_generation_);
        // INSTALL_DTV(tcb, raw) stores raw + one entry in tcbhead_t.dtv.
        mem.store<uint64_t>(tcb, dtv);
        for (const auto& obj : objects_) {
            if (!obj.tls.present || obj.tls.memsz == 0 ||
                obj.tls_mod_id == 0 || obj.tls_mod_id > capacity ||
                (obj.tls_dynamic && !obj.tls_static_slot)) continue;
            const uint64_t entry = dtv + obj.tls_mod_id * 16;
            if (mem.load<uint64_t>(entry) == 0) {
                const uint64_t tls_addr = static_cast<uint64_t>(
                    static_cast<int64_t>(tcb) + obj.tls_tp_offset);
                mem.store<uint64_t>(entry, tls_addr);
                mem.store<uint64_t>(entry + 8, 0);
            }
        }
    } catch (...) {
        return false;
    }
    return true;
}
uint64_t DynamicLinker::resolve_tls_address(Memory& mem, CPU& cpu,
                                             uint64_t module_id,
                                             uint64_t offset) {
    std::lock_guard<std::recursive_mutex> lk(loader_lock());
    if (module_id == 0 || cpu.tpidr_el0 == 0) return 0;
    LoadedObject* module = nullptr;
    for (auto& obj : objects_) {
        if (obj.tls_mod_id == module_id && obj.tls.present && obj.tls.memsz) {
            module = &obj;
            break;
        }
    }
    if (!module || offset > module->tls.memsz) return 0;
    const uint64_t tcb = cpu.tpidr_el0;
    if (is_musl_) {
        if (module->tls_dynamic) return 0;
        return static_cast<uint64_t>(static_cast<int64_t>(tcb) +
                                     module->tls_tp_offset) + offset;
    }
    if (!ensure_thread_dtv_capacity_(mem, tcb, module_id)) return 0;
    uint64_t dtv = 0;
    try { dtv = mem.load<uint64_t>(tcb); } catch (...) { return 0; }
    const uint64_t entry = dtv + module_id * 16;
    uint64_t tls_addr = 0;
    try { tls_addr = mem.load<uint64_t>(entry); } catch (...) { return 0; }
    if (tls_addr == 0) {
        if (module->tls_dynamic && !module->tls_static_slot) {
            uint64_t alignment = module->tls.align ? module->tls.align : 1;
            if ((alignment & (alignment - 1)) != 0) alignment = 1;
            if (module->tls.memsz > UINT64_MAX - (alignment - 1) ||
                module->tls.memsz > static_cast<uint64_t>(SIZE_MAX)) return 0;
            const uint64_t alloc_size = module->tls.memsz + alignment - 1;
            const uint64_t allocation = mem.mmap_alloc(alloc_size);
            if (allocation == 0) return 0;
            if (allocation > UINT64_MAX - (alignment - 1)) {
                mem.untrack_allocation(allocation, alloc_size);
                return 0;
            }
            tls_addr = (allocation + alignment - 1) & ~(alignment - 1);
            try {
                std::vector<uint8_t> zeros(static_cast<size_t>(module->tls.memsz), 0);
                mem.write(tls_addr, zeros.data(), zeros.size());
                if (module->tls.filesz > 0) {
                    if (module->tls.filesz > module->tls.memsz ||
                        module->tls_template_addr == 0) {
                        mem.untrack_allocation(allocation, alloc_size);
                        return 0;
                    }
                    std::vector<uint8_t> image(static_cast<size_t>(module->tls.filesz));
                    mem.read(module->tls_template_addr, image.data(), image.size());
                    mem.write(tls_addr, image.data(), image.size());
                }
                track_tls_allocation_(tcb, allocation, alloc_size,
                                      module->tls_mod_id);
            } catch (...) {
                mem.untrack_allocation(allocation, alloc_size);
                return 0;
            }
        } else {
            if (module->tls_template_addr == 0) return 0;
            tls_addr = static_cast<uint64_t>(static_cast<int64_t>(tcb) +
                                             module->tls_tp_offset);
            if (!mem.is_mapped(tls_addr, module->tls.memsz)) return 0;
            try {
                std::vector<uint8_t> image(static_cast<size_t>(module->tls.memsz));
                mem.read(module->tls_template_addr, image.data(), image.size());
                mem.write(tls_addr, image.data(), image.size());
            } catch (...) {
                return 0;
            }
        }
        try {
            mem.store<uint64_t>(entry, tls_addr);
            mem.store<uint64_t>(entry + 8, 0); // guest must not free emulator mappings
        } catch (...) {
            return 0;
        }
    }
    try { mem.store<uint64_t>(dtv, tls_generation_); } catch (...) {}
    return tls_addr + offset;
}
int64_t DynamicLinker::resolve_tlsdesc_offset(Memory& mem, CPU& cpu,
                                               uint64_t descriptor_arg) {
    std::lock_guard<std::recursive_mutex> lk(loader_lock());
    auto it = tlsdesc_indices_.find(descriptor_arg);
    if (it == tlsdesc_indices_.end()) return 0;
    // An undefined weak TLSDESC yields the relocation addend as an
    // absolute address (normally NULL), independently of the calling TP.
    if (it->second.module_id == 0)
        return static_cast<int64_t>(it->second.offset - cpu.tpidr_el0);
    const uint64_t addr = resolve_tls_address(mem, cpu,
                                              it->second.module_id,
                                              it->second.offset);
    if (addr == 0 || cpu.tpidr_el0 == 0) return 0;
    return static_cast<int64_t>(addr) - static_cast<int64_t>(cpu.tpidr_el0);
}
void DynamicLinker::deallocate_thread_tls(Memory& mem, uint64_t tcb) {
    if (tcb == 0) return;
    std::lock_guard<std::recursive_mutex> lk(loader_lock());
    auto it = tls_thread_allocations_.find(tcb);
    if (it != tls_thread_allocations_.end()) {
        for (const auto& alloc : it->second)
            mem.untrack_allocation(alloc.addr, alloc.size);
        tls_thread_allocations_.erase(it);
    }
    // A stack-cached pthread block may be reused after glibc calls this
    // shim. Clear its DTV pointer so initialize_thread_dtv rebuilds the
    // inline DTV instead of following an external DTV we just released.
    if (!is_musl_) {
        try {
            if (mem.is_mapped(tcb, sizeof(uint64_t)))
                mem.store<uint64_t>(tcb, 0);
        } catch (...) {}
    }
    tls_thread_blocks_.erase(tcb);
}
void DynamicLinker::register_thread_tls_block(uint64_t tcb) {
    if (tcb == 0) return;
    std::lock_guard<std::recursive_mutex> lk(loader_lock());
    tls_thread_blocks_.insert(tcb);
}
// ── parse_tls ──────────────────────────────────────────────────────────
void DynamicLinker::parse_tls(const std::vector<uint8_t>& data,
                              LoadedObject& obj) {
    if (data.size() < 64) return;
    uint64_t e_phoff;
    uint16_t e_phentsize, e_phnum;
    memcpy(&e_phoff,     data.data() + 32, 8);
    memcpy(&e_phentsize, data.data() + 54, 2);
    memcpy(&e_phnum,     data.data() + 56, 2);
    for (int i = 0; i < e_phnum; i++) {
        if (e_phoff + (i + 1) * e_phentsize > data.size()) break;
        const uint8_t* p = data.data() + e_phoff + i * e_phentsize;
        uint32_t p_type;
        memcpy(&p_type, p + 0, 4);
        if (p_type != 7) continue;  // PT_TLS = 7
        obj.tls.vaddr  = obj.base_addr + 0;  // relative to base; we'll add base when copying
        memcpy(&obj.tls.vaddr,  p + 16, 8);  // p_vaddr (relative, NOT adjusted by base)
        memcpy(&obj.tls.filesz, p + 32, 8);
        memcpy(&obj.tls.memsz,  p + 40, 8);
        memcpy(&obj.tls.align,  p + 48, 8);
        if (obj.tls.align == 0) obj.tls.align = 1;
        // ELF p_align is required to be 0, 1, or a power of two. Treat an
        // invalid value as byte alignment so malformed images cannot trigger
        // underflow in the alignment masks used by TLS placement.
        if ((obj.tls.align & (obj.tls.align - 1)) != 0)
            obj.tls.align = 1;
        if (obj.tls.filesz > obj.tls.memsz) return;
        obj.tls.present = true;
        return;
    }
}
// ── allocate_static_tls ────────────────────────────────────────────────
// Lay out each PT_TLS block using AArch64 glibc's variant-I TLS layout:
//   - Main exe TLS: at POSITIVE TP offsets (TP + tcb_size .. TP + tcb_size + main_memsz)
//   - Shared lib TLS: at NEGATIVE TP offsets (TP - lib_size .. TP)
//
// This matches what glibc expects on AArch64:
//   - Local-exec TLS access (mrs tpidr_el0; add x0, x0, #:tprel_hi:sym;
//     add x0, x0, #:tprel_lo12:sym) uses a POSITIVE offset baked at link
//     time = st_value + TLS_TCB_SIZE. So the main exe's TLS block MUST
//     be at TP + TLS_TCB_SIZE (positive offset from TP).
//   - The TCB header (tcbhead_t) occupies [TP, TP + TLS_TCB_SIZE). glibc's
//     create_thread fills in tcb/dtv/self/stack_guard/pointer_guard here.
//   - Shared lib TLS (accessed via __tls_get_addr / initial-exec for libs)
//     is at negative TP offsets, below the TCB.
//
// static_tls_base_ block layout (the "template" we copy per-thread):
//   [base .. base + lib_size)                          — lib TLS (negative TP)
//   [base + lib_size .. base + lib_size + tcb_size)    — TCB header (TP+0..TP+tcb_size)
//   [base + lib_size + tcb_size .. base + total)       — main exe TLS (positive TP)
// TP = base + lib_size  (points to the TCB header start)
//
// Per-thread (TP = tcb):
//   [tcb - lib_size .. tcb)                — lib TLS
//   [tcb .. tcb + tcb_size)                — TCB header
//   [tcb + tcb_size .. tcb + tcb_size + main_memsz) — main exe TLS
//
// offsets, TP = base + total). This broke local-exec TLS access for the
// main exe: the binary's hardcoded positive TPREL offset (e.g. +0x20)
// landed in the TCB header area instead of the main exe's TLS block.
// With small main TLS (8 bytes) and lucky alignment, this sometimes
// worked by accident. With larger main TLS (64+ bytes, e.g. __thread
// long tls_array[8]), the TLS data overlapped glibc's TCB fields and
// got clobbered, causing the "got = expected/4" TLS corruption pattern
// across 8+ threads.
void DynamicLinker::allocate_static_tls() {
    if (static_tls_base_ != 0) return;  // already allocated
    // First pass: compute lib_size and main TLS info.
    uint64_t lib_size = 0;
    uint64_t main_memsz = 0;
    uint64_t main_align = 1;
    uint64_t tls_module_count = 0;
    tls_static_alignment_ = 16;
    for (const auto& obj : objects_) {
        if (!obj.tls.present || obj.tls.memsz == 0) continue;
        ++tls_module_count;
        if (!is_musl_)
            tls_static_alignment_ = std::max(tls_static_alignment_, obj.tls.align);
        if (obj.is_main) {
            main_memsz = obj.tls.memsz;
            main_align = obj.tls.align ? obj.tls.align : 16;
        } else {
            uint64_t a = obj.tls.align ? obj.tls.align : 16;
            lib_size = (lib_size + a - 1) & ~(a - 1);
            lib_size += obj.tls.memsz;
        }
    }
    // Keep a minimal variant-I block for glibc even when the executable and
    // startup DSOs have no PT_TLS. A later dlopen can then use the same TP,
    // DTV, rseq slot, and static surplus as a process that had startup TLS.
    if (lib_size == 0 && main_memsz == 0 && is_musl_) return;
    // Round lib_size up to 16 (minimum TLS alignment).
    lib_size = (lib_size + 15) & ~15ULL;
    if (is_musl_) {
        // ── Variant-II (musl): ALL TLS at negative TP offsets ──────
        // TP = base + total (points PAST the block).
        // All modules' tp_offset = cursor - total (negative).
        // This is the original layout that worked for musl.
        uint64_t total = 0;
        uint64_t max_align = 16;
        for (auto& obj : objects_) {
            if (!obj.tls.present || obj.tls.memsz == 0) continue;
            if (obj.tls.align > max_align) max_align = obj.tls.align;
            total = (total + obj.tls.align - 1) & ~(obj.tls.align - 1);
            obj.tls_mod_id = next_tls_mod_id_++;
            total += obj.tls.memsz;
        }
        total = (total + max_align - 1) & ~(max_align - 1);
        static_tls_size_ = total;
        lib_tls_size_ = total;  // variant-II: all TLS is "negative TP"
        tcb_size_ = 0;          // no TCB header for musl variant-II
        static_tls_base_ = mem_.mmap_alloc(total + 16);
        if (static_tls_base_ == 0) {
            error_ = "failed to allocate static TLS block";
            return;
        }
        uint64_t cursor = 0;
        for (auto& obj : objects_) {
            if (!obj.tls.present || obj.tls.memsz == 0) continue;
            cursor = (cursor + obj.tls.align - 1) & ~(obj.tls.align - 1);
            obj.tls_block_offset = cursor;
            obj.tls_template_addr = static_tls_base_ + cursor;
            obj.tls_tp_offset = static_cast<int64_t>(cursor) -
                                static_cast<int64_t>(total);
            uint64_t src = obj.base_addr + obj.tls.vaddr;
            uint64_t dst = static_tls_base_ + cursor;
            if (obj.tls.filesz > 0) {
                try {
                    std::vector<uint8_t> buf(obj.tls.filesz);
                    mem_.read(src, buf.data(), buf.size());
                    mem_.write(dst, buf.data(), buf.size());
                } catch (...) {}
            }
            cursor += obj.tls.memsz;
        }
        return;
    }
    // ── Variant-I (glibc AArch64) ──────────────────────────────────
    // Main exe TLS at POSITIVE TP offsets, lib TLS at NEGATIVE TP offsets.
    // TCB header (tcbhead_t) at [TP, TP + tcb_size).
    //
    // See the long comment above for the full rationale.
    constexpr uint64_t TLS_TCB_SIZE_BASE = 0x10;  // sizeof(tcbhead_t) = tcb + dtv
    uint64_t tcb_size = (TLS_TCB_SIZE_BASE + main_align - 1) & ~(main_align - 1);
    // Reserve rseq and DTV storage after the startup TLS data, followed by
    // the dlopen surplus. All three regions have the same TP-relative
    // offsets on the main thread and on new pthread stacks.
    lib_tls_size_ = lib_size + TLS_PRE_TCB_SIZE;
    tcb_size_ = tcb_size;
    // Total static TLS block = lib + struct pthread + TCB + main.
    uint64_t total = lib_size + TLS_PRE_TCB_SIZE + tcb_size + main_memsz;
    // Round up to max alignment (16 minimum).
    uint64_t max_align = 16;
    if (main_align > max_align) max_align = main_align;
    total = (total + max_align - 1) & ~(max_align - 1);
    // ld-linux places its rseq area after startup TLS, with at least 32-byte
    // alignment. Reserve that area in the positive slack before dlopen TLS so
    // glibc's TP + __rseq_offset writes cannot overlap the DTV or a later TLS
    // module. mmap_alloc returns a page-aligned base, so aligning this template
    // offset also aligns the area for the main thread and every allocated TCB.
    const uint64_t rseq_slot = (total + TLS_RSEQ_AREA_SIZE - 1) &
                               ~(TLS_RSEQ_AREA_SIZE - 1);
    rseq_offset_ = rseq_slot - (lib_size + TLS_PRE_TCB_SIZE);
    dtv_capacity_ = tls_module_count + TLS_DTV_SURPLUS;
    const uint64_t dtv_slot = (rseq_slot + TLS_RSEQ_AREA_SIZE + 15) & ~15ULL;
    dtv_storage_offset_ = dtv_slot - (lib_size + TLS_PRE_TCB_SIZE);
    const uint64_t dtv_bytes = (dtv_capacity_ + 2) * sizeof(uint64_t) * 2;
    tls_surplus_base_ = (dtv_slot + dtv_bytes + 15) & ~15ULL;
    // static_tls_size_ covers both reservations, so GLRO-sized guest thread
    // blocks (0x1001 caller-alloc path) fit positive-tp dlopen slots.
    static_tls_size_ = tls_surplus_base_ + TLS_DLOPEN_SURPLUS;
    const uint64_t tls_align_slack = tls_static_alignment_ - 1;
    if (static_tls_size_ > UINT64_MAX - tls_align_slack) {
        error_ = "static TLS block size overflow";
        return;
    }
    static_tls_alloc_size_ = static_tls_size_ + tls_align_slack;
    static_tls_alloc_base_ = mem_.mmap_alloc(static_tls_alloc_size_);
    if (static_tls_alloc_base_ == 0) {
        error_ = "failed to allocate static TLS block";
        return;
    }
    static_tls_base_ = (static_tls_alloc_base_ + tls_align_slack) &
                       ~tls_align_slack;
    // TP = static_tls_base_ + lib_size  (points to the TCB header start).
    // Main exe TLS is at TP + tcb_size (positive offset).
    // Lib TLS is at TP - lib_size (negative offset).
    // Second pass: assign module IDs, tp_offsets, block_offsets, and copy
    // .tdata templates.
    uint64_t lib_cursor = 0;   // offset within [base, base+lib_size)
    for (auto& obj : objects_) {
        if (!obj.tls.present || obj.tls.memsz == 0) continue;
        obj.tls_mod_id = next_tls_mod_id_++;
        if (obj.is_main) {
            // Main exe TLS: at POSITIVE TP offset = tcb_size.
            // In the template block it follows lib TLS + struct pthread.
            obj.tls_block_offset = lib_size + TLS_PRE_TCB_SIZE + tcb_size;
            obj.tls_template_addr = static_tls_base_ + obj.tls_block_offset;
            obj.tls_tp_offset = static_cast<int64_t>(tcb_size);  // positive
        } else {
            // Lib TLS: at NEGATIVE TP offset.
            // Align lib_cursor.
            uint64_t a = obj.tls.align ? obj.tls.align : 16;
            lib_cursor = (lib_cursor + a - 1) & ~(a - 1);
            // In the template block, lib TLS is at [base + lib_cursor, ...).
            // But we store libs in REVERSE order so that the first lib loaded
            // (libc, objects_[1]) is closest to TP (smallest negative offset).
            // Actually, for simplicity, store libs in load order at increasing
            // negative offsets. The tp_offset = lib_cursor - lib_size (negative).
            obj.tls_block_offset = lib_cursor;
            obj.tls_template_addr = static_tls_base_ + obj.tls_block_offset;
            obj.tls_tp_offset = static_cast<int64_t>(lib_cursor) -
                                static_cast<int64_t>(lib_tls_size_);  // negative
            lib_cursor += obj.tls.memsz;
        }
        // Copy initialized data (.tdata) from obj's PT_TLS filesz.
        uint64_t src = obj.base_addr + obj.tls.vaddr;
        uint64_t dst = static_tls_base_ + obj.tls_block_offset;
        if (obj.tls.filesz > 0) {
            try {
                std::vector<uint8_t> buf(obj.tls.filesz);
                mem_.read(src, buf.data(), buf.size());
                mem_.write(dst, buf.data(), buf.size());
            } catch (...) {
                // Reading the source failed — leave zeroed (mmap gave us zeros).
            }
        }
        // .bss (memsz - filesz) is already zero from mmap.
    }
    if (!initialize_thread_dtv(mem_, thread_pointer())) {
        error_ = "failed to initialize main-thread DTV";
    }
}
// ── TLS accessors ──────────────────────────────────────────────────────
uint64_t DynamicLinker::tls_mod_id(const std::string& name) const {
    for (const auto& o : objects_) {
        if (o.name == name) return o.tls_mod_id;
    }
    return 0;
}
int64_t DynamicLinker::tls_tp_offset(uint64_t mod_id) const {
    for (const auto& o : objects_) {
        if (o.tls_mod_id == mod_id) return o.tls_tp_offset;
    }
    return 0;
}
// ── allocate_thread_tls ────────────────────────────────────────────────
// Allocate a fresh per-thread TLS block (variant-I glibc layout) for a
// new guest thread spawned outside clone() (e.g. SDL_CreateThread via the
// thunk). Mirrors the a0==0 path of the 0x1001 _dl_allocate_tls syscall
// (Source/System/Syscalls/HostCalls/DynamicLinker.cpp) — see that handler for the full layout
// description. Layout:
//   [block .. block+lib_size)          = lib TLS (negative TP offsets)
//   [block+lib_size .. block+lib_size+tcb_size) = TCB header
//   [block+lib_size+tcb_size .. total) = main exe TLS (positive TP offsets)
// Returns the TCB pointer (== TPIDR_EL0), or 0 on failure.
uint64_t DynamicLinker::allocate_thread_tls(Memory& mem) {
    std::lock_guard<std::recursive_mutex> lk(loader_lock());
    uint64_t lib_size = 0;
    uint64_t main_memsz = 0;
    uint64_t main_align = 1;
    for (const auto& obj : objects_) {
        if (!obj.tls.present || obj.tls.memsz == 0) continue;
        // Dynamic (dlopen'd) libs live in the startup surplus, outside the
        // startup lib area: excluding them keeps the recomputed lib_size
        // equal to the frozen startup anchor (lib_tls_size_), so new
        // threads get the same TP as the main thread. Their template bytes
        // are still copied below via stored block offsets.
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
    // No surplus added here: the TP anchor is the frozen startup lib area
    // (lib_tls_size_); surplus lives trailing (positive tp) outside it.
    // Dynamic libs are skipped above so the recompute matches exactly.
    constexpr uint64_t TLS_TCB_SIZE_BASE = 0x10;  // sizeof(tcbhead_t)={dtv,private} per upstream sysdeps/aarch64/nptl/tls.h
    uint64_t tcb_size = (main_align > 1)
        ? (TLS_TCB_SIZE_BASE + main_align - 1) & ~(main_align - 1)
        : TLS_TCB_SIZE_BASE;
    uint64_t total_tls_size = lib_size + TLS_PRE_TCB_SIZE + tcb_size + main_memsz;
    uint64_t tcb;
    if (total_tls_size == 0) {
        // No TLS — return a minimal zeroed block.
        constexpr uint64_t FALLBACK_SIZE = 4096;
        uint64_t block = mem.mmap_alloc(FALLBACK_SIZE);
        if (block == 0) return 0;
        std::vector<uint8_t> zeros(FALLBACK_SIZE, 0);
        mem.write(block, zeros.data(), FALLBACK_SIZE);
        tcb = (block + FALLBACK_SIZE - 16) & ~0xFULL;
        return tcb;
    }
    constexpr uint64_t PTHREAD_SLACK = 8192;
    const uint64_t tls_extent = std::max(total_tls_size, static_tls_size_);
    const uint64_t alignment = std::max<uint64_t>(16, tls_static_alignment_);
    if (tls_extent > UINT64_MAX - PTHREAD_SLACK - (alignment - 1) - 63)
        return 0;
    uint64_t alloc_size = tls_extent + PTHREAD_SLACK + alignment - 1;
    alloc_size = (alloc_size + 63) & ~63ULL;
    uint64_t block = mem.mmap_alloc(alloc_size);
    if (block == 0) return 0;
    std::vector<uint8_t> zeros(alloc_size, 0);
    mem.write(block, zeros.data(), alloc_size);
    const uint64_t tls_base = (block + alignment - 1) & ~(alignment - 1);
    tcb = tls_base + lib_size + TLS_PRE_TCB_SIZE; // TCB above struct pthread
    // ── Copy each module's TLS template to its per-thread slot ──
    for (const auto& obj : objects_) {
        if (!obj.tls.present || obj.tls.memsz == 0) continue;
        if (obj.tls_dynamic && !obj.tls_static_slot) continue;
        int64_t tp_off = obj.tls_tp_offset;
        uint64_t dst = static_cast<uint64_t>(
            static_cast<int64_t>(tcb) + tp_off);
        uint64_t src = obj.tls_template_addr;
        uint64_t filesz = obj.tls.filesz;
        if (filesz > 0 && filesz <= obj.tls.memsz) {
            try {
                std::vector<uint8_t> tpl(filesz);
                mem.read(src, tpl.data(), filesz);
                mem.write(dst, tpl.data(), filesz);
            } catch (...) {}
        }
    }
    // AArch64 glibc's tcbhead_t is { dtv, private } at TP+0/+8.
    try {
        mem.store<uint64_t>(tcb + 8, 0);
    } catch (...) {}
    // SDL-created threads may predate the first late TLS module in a static
    // executable. Their allocation includes pthread slack, so the first
    // later TLS lookup can place the DTV after the layout is established.
    if (static_tls_base_ != 0 && !initialize_thread_dtv(mem, tcb)) return 0;
    register_thread_tls_block(tcb);
    return tcb;
}
// ── tlsdesc_tp_offset ──────────────────────────────────────────────────
// Compute the TP offset for an R_AARCH64_TLSDESC relocation. The subtle
// case is an *imported* TLS symbol: the relocation lives in one module
// (e.g. libicuuc's GOT) but the symbol is defined in another (libstdc++).
// The importing module's .dynsym st_value is 0 for undefined symbols, so
// we must resolve to the *defining* module and use ITS TLS block offset +
// ITS st_value. Using the importing module's st_value silently shifted
// every cross-module TLS access to the importing module's block, so
// std::call_once's __once_call store landed on __once_callable's slot
// (both appeared to resolve to the same negative offset) and __once_proxy
// read 0 → br x0 → crash at pc=0x0.
int64_t DynamicLinker::tlsdesc_tp_offset(const LoadedObject& obj,
                                         uint32_t sym_idx, int64_t A) {
    TlsIndex index{};
    if (!resolve_tlsdesc_index_(obj, sym_idx, A, index) ||
        index.offset > static_cast<uint64_t>(INT64_MAX))
        return 0;
    for (const auto& candidate : objects_) {
        if (candidate.tls_mod_id != index.module_id) continue;
        if (candidate.tls_tp_offset >
            INT64_MAX - static_cast<int64_t>(index.offset)) return 0;
        return candidate.tls_tp_offset + static_cast<int64_t>(index.offset);
    }
    return 0;
}
bool DynamicLinker::resolve_tlsdesc_index_(const LoadedObject& obj,
                                            uint32_t sym_idx,
                                            int64_t addend,
                                            TlsIndex& out) {
    const LoadedObject* defining = &obj;
    uint64_t symbol_offset = 0;
    if (sym_idx != 0) {
        Elf64_Sym sym{};
        try {
            mem_.read(obj.symtab_addr + sym_idx * sizeof(sym), &sym, sizeof(sym));
        } catch (...) {
            return false;
        }
        if ((sym.st_info & 0x0f) != 6) return false; // STT_TLS
        const uint8_t bind = sym.st_info >> 4;
        const uint8_t visibility = sym.st_other & 0x3;
        if (sym.st_shndx != SHN_UNDEF_ &&
            (bind == 0 || visibility == 2 || visibility == 3)) {
            // Local, hidden, and protected TLS definitions bind within the
            // defining object even when another object exports the same name.
            symbol_offset = sym.st_value;
        } else {
            const uint64_t symbol_addr = resolve_reloc_symbol(obj, sym_idx);
            if (symbol_addr != 0) {
                defining = find_object_by_addr(symbol_addr);
                if (!defining || symbol_addr < defining->base_addr) return false;
                symbol_offset = symbol_addr - defining->base_addr;
            } else if (sym.st_shndx != SHN_UNDEF_) {
                // A defined, preemptible symbol may not have been indexed
                // (for example, a local static-PIE object); keep its own
                // definition as the relocation's fallback.
                symbol_offset = sym.st_value;
            } else {
                return false;
            }
        }
    }
    if (!defining->tls.present || defining->tls.memsz == 0 ||
        defining->tls_mod_id == 0) return false;
    uint64_t offset = symbol_offset;
    if (addend >= 0) {
        const uint64_t positive = static_cast<uint64_t>(addend);
        if (positive > UINT64_MAX - offset) return false;
        offset += positive;
    } else {
        const uint64_t magnitude = static_cast<uint64_t>(-(addend + 1)) + 1;
        if (magnitude > offset) return false;
        offset -= magnitude;
    }
    if (offset > defining->tls.memsz) return false;
    out.module_id = defining->tls_mod_id;
    out.offset = offset;
    return true;
}
bool DynamicLinker::install_tlsdesc_(const LoadedObject& obj,
                                     uint32_t sym_idx,
                                     int64_t addend,
                                     uint64_t target) {
    TlsIndex index{};
    if (!resolve_tlsdesc_index_(obj, sym_idx, addend, index)) {
        // A missing weak TLS definition is valid. Use the syscall-backed
        // resolver because its addend - TP result is thread-dependent.
        Elf64_Sym symbol{};
        bool undefined_weak = false;
        if (sym_idx != 0) {
            try {
                mem_.read(obj.symtab_addr + sym_idx * sizeof(symbol),
                          &symbol, sizeof(symbol));
                undefined_weak = (symbol.st_info & 0xf) == 6 &&
                    (symbol.st_info >> 4) == STB_WEAK_ &&
                    symbol.st_shndx == SHN_UNDEF_ &&
                    resolve_reloc_symbol(obj, sym_idx) == 0;
            } catch (...) {}
        }
        if (undefined_weak) {
            try {
                tlsdesc_indices_[target] = {0, static_cast<uint64_t>(addend)};
                mem_.store<uint64_t>(target, shim_base_ + SHIM_CODE_PAGE_OFF_ +
                                            tlsdesc_dynamic_resolver_off_);
                mem_.store<uint64_t>(target + 8, 0);
                return true;
            } catch (...) {
                tlsdesc_indices_.erase(target);
            }
        }
        set_last_error("could not resolve TLS descriptor in " + obj.name);
        error_ = "unresolved TLS descriptor in " + obj.name;
        try {
            mem_.store<uint64_t>(target, 0);
            mem_.store<uint64_t>(target + 8, 0);
        } catch (...) {}
        return false;
    }
    const LoadedObject* defining = nullptr;
    for (const auto& candidate : objects_) {
        if (candidate.tls_mod_id == index.module_id) {
            defining = &candidate;
            break;
        }
    }
    if (!defining) {
        set_last_error("TLS descriptor module disappeared in " + obj.name);
        error_ = "TLS descriptor module missing in " + obj.name;
        return false;
    }
    const bool needs_dynamic_resolver =
        defining->tls_dynamic && !defining->tls_static_slot;
    const uint64_t code_base = shim_base_ + SHIM_CODE_PAGE_OFF_;
    const uint64_t resolver = code_base + (needs_dynamic_resolver
        ? tlsdesc_dynamic_resolver_off_ : tlsdesc_resolver_off_);
    uint64_t descriptor_arg = 0;
    if (needs_dynamic_resolver) {
        tlsdesc_indices_[target] = index;
        descriptor_arg = 0; // resolver identifies this descriptor by x0
    } else {
        if (index.offset > static_cast<uint64_t>(INT64_MAX) ||
            defining->tls_tp_offset >
                INT64_MAX - static_cast<int64_t>(index.offset)) {
            set_last_error("TLS descriptor offset overflow in " + obj.name);
            error_ = "TLS descriptor offset overflow in " + obj.name;
            return false;
        }
        const int64_t tp_offset = defining->tls_tp_offset +
                                  static_cast<int64_t>(index.offset);
        descriptor_arg = static_cast<uint64_t>(tp_offset);
        tlsdesc_indices_.erase(target);
    }
    try {
        mem_.store<uint64_t>(target, resolver);
        mem_.store<uint64_t>(target + 8, descriptor_arg);
    } catch (...) {
        set_last_error("could not write TLS descriptor in " + obj.name);
        error_ = "could not write TLS descriptor in " + obj.name;
        return false;
    }
    if (dynlink_trace_enabled()) {
        fprintf(stderr, "[dynlink] TLSDESC obj=%s @0x%llx module=%llu offset=0x%llx %s resolver=0x%llx\n",
                obj.name.c_str(), (unsigned long long)target,
                (unsigned long long)index.module_id,
                (unsigned long long)index.offset,
                needs_dynamic_resolver ? "dynamic" : "static",
                (unsigned long long)resolver);
    }
    return true;
}
}  // namespace arm64emu
