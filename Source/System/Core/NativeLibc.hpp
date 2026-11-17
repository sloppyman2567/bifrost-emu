// NativeLibc.hpp — host-native versions of the guest libc's bulk memory routines.
//
// A guest memset or memcpy of tens of kilobytes is a loop of vector stores that the JIT guards one by one; the host's
// own routine does the same work several times faster. The loader records where the guest's memcpy, memmove and memset
// start (NativeLibcRoutines::add, from the dynamic linker's symbol table and from a static executable's .symtab), the
// JIT puts a size check and a call to `run` at the top of the block that starts there, and `run` either performs the
// whole routine and returns as the guest's `ret` would, or declines and leaves the guest's own routine to run.
//
// `run` declines (returns false) whenever it cannot prove the exact guest behaviour: a size below kNativeLibcMinBytes,
// a range that is not entirely mapped with the right access (so every fault, userfaultfd page and protection error
// happens in the guest routine, at the guest's address), and a memcpy whose ranges overlap. Nothing is written before
// the checks finish.
#pragma once
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace arm64emu {

class CPU;
class Memory;

enum class NativeLibcKind : uint8_t { None = 0, Memcpy = 1, Memmove = 2, Memset = 3 };

// Below this the guest's own routine is as fast as leaving generated code for the host's: the exit and the call cost
// about 33 ns, and the guest routine ties them at 512 bytes (memmove loses a little) and loses from 1 KiB up
// (measured 2026-11-17 on a Ryzen 7 5800X3D, docs/archive/JIT-history.md). A size, not a ratio.
constexpr uint64_t kNativeLibcMinBytes = 1024;

class NativeLibcRoutines {
public:
    // memcpy, memmove, memset by exact name; None for anything else.
    static NativeLibcKind kind_of(const std::string& name);
    // Remember that the guest routine of `kind` starts at `pc`. Safe from any thread.
    void add(uint64_t pc, NativeLibcKind kind);
    NativeLibcKind find(uint64_t pc) const;
    // An execve replaces the image: every address recorded for the old one is meaningless.
    void clear();
    bool empty() const { return count_.load(std::memory_order_relaxed) == 0; }
    // The routine the guest just entered (arguments in x0-x2, return address in x30). True when it ran and the CPU
    // looks as if the guest's `ret` had executed (x0 = the destination, pc = x30); false leaves the CPU untouched.
    bool run(Memory& mem, CPU& cpu, NativeLibcKind kind);
    uint64_t calls() const { return calls_.load(std::memory_order_relaxed); }
    uint64_t bytes() const { return bytes_.load(std::memory_order_relaxed); }

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<uint64_t, NativeLibcKind> by_pc_;
    std::atomic<size_t> count_{0};
    std::atomic<uint64_t> calls_{0}, bytes_{0};
};

// Record the memcpy/memmove/memset functions in an ELF image's .symtab (an unstripped static executable has no
// dynamic symbols for them). `load_base` is where the image was mapped. IFUNC symbols are skipped: their value is
// the resolver, not the routine.
void register_symtab_routines(NativeLibcRoutines& table, const std::vector<uint8_t>& elf, uint64_t load_base);

}  // namespace arm64emu
