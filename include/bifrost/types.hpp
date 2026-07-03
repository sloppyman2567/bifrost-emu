// bifrost/types.hpp — common types shared across the bifrost-emu public API.
//
// This header is the "root" of the public include tree. It provides:
//   - Forward declarations of every major class (Memory, CPU, Emulator,
//     ElfLoader, FrostJIT, SignalTable, GraphicsBackend, DecodedInst)
//   - Exception hierarchy (EmuError / UnmappedMemory / DecodeError / SyscallError)
//   - Small utilities (sign_extend, is_power_of_two, ror64)
//   - The namespace `arm64emu`
//
// It deliberately has NO dependencies on internal headers and pulls in only
// the standard library + decoder.hpp (which is itself standalone).
#pragma once

#include "decoder.hpp"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace arm64emu {

// ── Forward declarations ──────────────────────────────────────────────
// The full class definitions live in their respective subdirectories:
//   Memory           — src/core/memory.{h,cpp}
//   CPU              — src/core/cpu.{h,cpp}
//   Emulator         — src/core/emulator.{h,cpp}  (public decl in bifrost/emulator.hpp)
//   ElfLoader        — src/frontend/elf_loader.cpp
//   FrostJIT         — src/jit/frostjit.{h,cpp}   (public decl in jit/frostjit.hpp)
//   SignalTable      — src/core/signal.{h,cpp}
//   GraphicsBackend  — include/graphics.hpp
//   IRBlock / IRInst — include/ir/ir.hpp
class Memory;
class CPU;
class Emulator;
class ElfLoader;
class FrostJIT;
class SignalTable;
class GraphicsBackend;
struct IRBlock;
struct IRInst;

// ── Utility ───────────────────────────────────────────────────────────
static inline uint64_t sign_extend(uint64_t v, int bits) {
    if (bits >= 64) return v;
    uint64_t m = 1ULL << (bits - 1);
    return (v ^ m) - m;
}

static inline bool is_power_of_two(uint64_t x) { return x && !(x & (x - 1)); }

static inline uint64_t ror64(uint64_t v, unsigned r) {
    r &= 63;
    // Shift by 64 is undefined behavior in C++; ARM ROR by 0 (or 64)
    // is a valid no-op encoding, so guard the r==0 case explicitly.
    if (r == 0) return v;
    return (v >> r) | (v << (64 - r));
}

// ── Exceptions ────────────────────────────────────────────────────────
struct EmuError : std::runtime_error { using std::runtime_error::runtime_error; };

struct UnmappedMemory : EmuError {
    uint64_t addr;   // faulting guest virtual address
    bool     write;  // true = write fault, false = read fault

    UnmappedMemory(uint64_t a, bool w)
        : EmuError(std::string("unmapped ") + (w ? "write" : "read") +
                   " at 0x" + [&]{
                       char b[32]; snprintf(b, sizeof(b), "%lx", a); return std::string(b);
                   }()),
          addr(a), write(w) {}
};

struct DecodeError : EmuError {
    DecodeError(uint64_t pc, uint32_t inst)
        : EmuError([&]{
              char b[64]; snprintf(b, sizeof(b),
                  "decode error at pc=0x%lx inst=0x%08x", pc, inst);
              return std::string(b);
          }()) {}
};

struct SyscallError : EmuError { using EmuError::EmuError; };

} // namespace arm64emu
