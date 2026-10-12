// Types.hpp — common types shared across the bifrost-emu public API.
//
// This header is the "root" of the public include tree. It provides:
//   - Forward declarations of every major class (Memory, CPU, Emulator,
//     ElfLoader, FrostJIT, SignalTable, GraphicsBackend, DecodedInst)
//   - Exception hierarchy (EmuError / UnmappedMemory / DecodeError / SyscallError)
//   - Small utilities (sign_extend, is_power_of_two, ror64)
//   - The namespace `arm64emu`
//
// It deliberately has NO dependencies on internal headers and pulls in only
// the standard library + Decoder.hpp (which is itself standalone).
#pragma once
#include "Interpreter/Decoder/Decoder.hpp"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
namespace arm64emu {
// ── Forward declarations ──────────────────────────────────────────────
// The full class definitions live in their respective subdirectories:
//   Memory           — Source/System/Memory/Memory.{hpp,cpp}
//   CPU              — Source/System/Core/CPU.{hpp,cpp}
//   Emulator         — Source/System/Core/Emulator/ (public decl in Bifrost/Emulator.hpp)
//   ElfLoader        — Source/System/Loader/ElfLoader.cpp
//   FrostJIT         — Source/FrostJIT/Compiler/FrostJIT.{hpp,cpp}
//   SignalTable      — Source/System/Core/Signals.{hpp,cpp}
//   GraphicsBackend  — Source/System/Graphics/Display/Graphics.hpp
//   IRBlock / IRInst — Source/FrostJIT/IR/IR.hpp
class Memory;
class CPU;
class Emulator;
class ElfLoader;
class FrostJIT;
class SignalTable;
class FrostGraphics;
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
    bool     protection;  // mapped page rejected the requested access
    bool     uffd = false;  // missing page in a userfaultfd SIGBUS-mode range
    // SIGSEGV (MAPERR/ACCERR), or SIGBUS BUS_ADRERR for a userfaultfd page.
    int signo() const { return uffd ? 7 : 11; }
    int segv_code() const { return uffd || protection ? 2 : 1; }
    UnmappedMemory(uint64_t a, bool w, bool p = false)
        : EmuError(std::string("unmapped ") + (w ? "write" : "read") +
                   " at 0x" + [&]{
                       char b[32]; snprintf(b, sizeof(b), "%lx", a); return std::string(b);
                   }()),
          addr(a), write(w), protection(p) {}
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
