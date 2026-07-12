// frost_graphics/thunk_common.hpp — shared trampoline encoding + registry
// helpers for the GraphicThunk / AudioThunk / DisplayThunk family.
//
// v1.5.0.alpha: extracted from GraphicThunk to avoid duplication now
// that we have three thunk classes.
//
// All three thunks:
//   1. Allocate a guest page for trampolines (16 bytes per symbol).
//   2. Each trampoline is:
//        movz x9, #sym_id        ; load symbol_id
//        movz x8, #0x1000        ; load __NR_bifrost_thunk
//        svc #0                  ; trap to host
//        nop                     ; pad to 16 bytes
//   3. The host's syscall dispatcher (case 0x1000 in misc.cpp) calls
//      thunk->dispatch(cpu, sym_id), which:
//        a. Looks up the host function pointer for sym_id.
//        b. Reads the first 8 args from cpu.regs[0..7].
//        c. Calls the host function with the args.
//        d. Writes the return value to cpu.regs[0].
//
// The dispatcher does NOT know which thunk class owns a given sym_id —
// it tries GraphicThunk first, then AudioThunk, then DisplayThunk.
// (Each thunk has its own symbol_id namespace, starting from 0.)
#pragma once

#include "core/cpu.h"
#include "core/memory.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace arm64emu {

// AArch64 instruction encodings for trampolines.
namespace trampoline_enc {
    constexpr uint32_t MOVZ_Xd_IMM16(int Xd, uint16_t imm16) {
        return 0xD2800000u | (static_cast<uint32_t>(imm16) << 5)
                            | (static_cast<uint32_t>(Xd) & 0x1Fu);
    }
    constexpr uint32_t SVC_0 = 0xD4000001u;
    constexpr uint32_t NOP   = 0xD503201Fu;
}

// Common registry entry. All three thunks use this same shape.
struct ThunkSymbolEntry {
    std::string name;       // e.g. "glClear" or "snd_pcm_open"
    void*       host_fn;    // host function pointer (or null if stub)
    uint64_t    guest_addr; // trampoline address in guest memory
    uint32_t    symbol_id;  // small int (0..MAX_SYMBOLS-1)
    // v1.5.0.alpha (Turn 74): bitmask indicating which args (0-7) are
    // pointers that need guest→host translation. Bit N set = arg N is
    // a pointer. 0 = no pointer args (all args passed verbatim).
    // This is populated per-symbol by the registration code using
    // the POINTER_ARGS macro in register_known_symbols_().
    uint8_t     pointer_args = 0;
};

// Write a 16-byte trampoline at the given guest address for the given
// sym_id. Used by all three thunks.
inline void write_thunk_trampoline(Memory& mem, uint64_t addr, uint32_t sym_id,
                                    uint16_t syscall_number) {
    uint32_t buf[4];
    buf[0] = trampoline_enc::MOVZ_Xd_IMM16(9, static_cast<uint16_t>(sym_id));
    buf[1] = trampoline_enc::MOVZ_Xd_IMM16(8, syscall_number);
    buf[2] = trampoline_enc::SVC_0;
    buf[3] = trampoline_enc::NOP;
    mem.write(addr, buf, sizeof(buf));
}

// Generic dispatch helper. Reads 8 args from cpu.regs[0..7], calls the
// host function as an 8-arg uint64_t function pointer, writes the
// return value to cpu.regs[0]. Returns 0 on success.
//
// All three thunks use this same dispatch logic because the calling
// convention is identical (SysV AMD64 for the host, AAPCS for the guest).
inline int64_t thunk_dispatch_generic(CPU& cpu, void* host_fn,
                                       const std::string& name,
                                       bool trace) {
    if (!host_fn) {
        if (trace) {
            fprintf(stderr, "[thunk] dispatch: %s (stub, returns 0)\n",
                    name.c_str());
        }
        cpu.regs[0] = 0;
        return 0;
    }
    uint64_t args[8];
    for (int i = 0; i < 8; i++) args[i] = cpu.regs[i];

    if (trace) {
        fprintf(stderr, "[thunk] dispatch: %s (host_fn=%p) "
                "a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx\n",
                name.c_str(), host_fn,
                static_cast<unsigned long long>(args[0]),
                static_cast<unsigned long long>(args[1]),
                static_cast<unsigned long long>(args[2]),
                static_cast<unsigned long long>(args[3]));
    }

    using GenericFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
    auto fn = reinterpret_cast<GenericFn>(host_fn);
    uint64_t ret = fn(args[0], args[1], args[2], args[3],
                       args[4], args[5], args[6], args[7]);
    cpu.regs[0] = ret;
    return 0;
}

// Turn 91: Pointer-aware dispatch for display thunks (Wayland/X11/Vulkan).
// Many Wayland/X11 functions take pointer args (const char* name,
// wl_proxy*, XEvent*, etc.) that need guest→host translation.
// This helper translates pointer args before calling the host function.
inline int64_t thunk_dispatch_with_ptrs(CPU& cpu, void* host_fn,
                                         const std::string& name,
                                         uint8_t pointer_args,
                                         Memory* mem, bool trace) {
    if (!host_fn) {
        if (trace) {
            fprintf(stderr, "[display-thunk] dispatch: %s (stub, returns 0)\n",
                    name.c_str());
        }
        cpu.regs[0] = 0;
        return 0;
    }
    uint64_t args[8];
    for (int i = 0; i < 8; i++) args[i] = cpu.regs[i];

    // Translate pointer args from guest to host.
    if (pointer_args && mem) {
        for (int i = 0; i < 8; i++) {
            if (pointer_args & (1u << i)) {
                if (args[i] == 0) continue;  // NULL is NULL
                uint8_t* host_ptr = mem->guest_to_host_ptr(args[i]);
                if (host_ptr) {
                    args[i] = reinterpret_cast<uint64_t>(host_ptr);
                }
                // If translation failed, pass original — host fn may handle
                // gracefully or crash (visible failure, not silent corruption).
            }
        }
    }

    if (trace) {
        fprintf(stderr, "[display-thunk] dispatch: %s (host_fn=%p) "
                "a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx ptrs=0x%x\n",
                name.c_str(), host_fn,
                static_cast<unsigned long long>(args[0]),
                static_cast<unsigned long long>(args[1]),
                static_cast<unsigned long long>(args[2]),
                static_cast<unsigned long long>(args[3]),
                pointer_args);
    }

    using GenericFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
    auto fn = reinterpret_cast<GenericFn>(host_fn);
    uint64_t ret = fn(args[0], args[1], args[2], args[3],
                       args[4], args[5], args[6], args[7]);
    cpu.regs[0] = ret;
    return 0;
}

// Per-library table. Each thunk maintains a vector of these.
struct ThunkLibTable {
    std::string lib;
    std::vector<ThunkSymbolEntry> entries;
};

// Find or create a LibTable for `lib`.
inline ThunkLibTable* find_or_create_lib(std::vector<ThunkLibTable>& libs,
                                          const std::string& lib) {
    for (auto& l : libs) {
        if (l.lib == lib) return &l;
    }
    libs.push_back({lib, {}});
    return &libs.back();
}

inline ThunkLibTable* find_lib(std::vector<ThunkLibTable>& libs,
                                const std::string& lib) {
    for (auto& l : libs) {
        if (l.lib == lib) return &l;
    }
    return nullptr;
}

// Register a (lib, sym, host_fn) entry. Allocates a sym_id, writes the
// trampoline into guest memory, stores the entry. Idempotent.
// v1.5.0.alpha (Turn 74): added id_base parameter so each thunk type
// (Graphic/Audio/Display) gets a non-overlapping symbol_id range.
// Turn 91: added pointer_args parameter for display thunk pointer translation.
inline void thunk_register(Memory& mem,
                            std::vector<ThunkLibTable>& libs,
                            std::vector<std::pair<uint32_t, uint32_t>>& id_to_idx,
                            uint64_t trampoline_base, uint64_t trampoline_size,
                            uint64_t max_symbols, uint16_t syscall_number,
                            uint32_t id_base, bool trace,
                            const std::string& lib,
                            const std::string& sym, void* host_fn,
                            uint8_t pointer_args = 0) {
    ThunkLibTable* lt = find_or_create_lib(libs, lib);
    for (const auto& e : lt->entries) {
        if (e.name == sym) return;  // idempotent
    }
    uint32_t local_id = static_cast<uint32_t>(id_to_idx.size());
    if (local_id >= max_symbols) {
        fprintf(stderr, "[thunk] register: symbol table full (%zu)\n",
                id_to_idx.size());
        return;
    }
    uint32_t sym_id = id_base + local_id;
    uint64_t addr = trampoline_base + local_id * trampoline_size;
    write_thunk_trampoline(mem, addr, sym_id, syscall_number);
    lt->entries.push_back({sym, host_fn, addr, sym_id, pointer_args});
    id_to_idx.push_back({
        static_cast<uint32_t>(std::distance(libs.data(), lt)),
        static_cast<uint32_t>(lt->entries.size() - 1)
    });
    if (trace) {
        fprintf(stderr, "[thunk] registered %s:%s -> 0x%llx (id=%u ptrs=0x%x)\n",
                lib.c_str(), sym.c_str(),
                static_cast<unsigned long long>(addr), sym_id, pointer_args);
    }
}

} // namespace arm64emu
