// GuestDebugger — cooperative AArch64 ptrace within one fork family.
// Shared control lives outside guest memory; only guest tasks registered in
// this domain can be traced. Host ptrace/registers are never exposed.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
namespace arm64emu {
class CPU;
class Emulator;
struct GuestDebugControl { std::atomic<uint32_t> work{0}; };
class GuestDebugger {
public:
    explicit GuestDebugger(Emulator& emu);
    ~GuestDebugger();
    GuestDebugger(const GuestDebugger&) = delete;
    GuestDebugger& operator=(const GuestDebugger&) = delete;
    bool register_task(CPU& cpu);
    void before_fork();
    void fork_return(bool child);
    void after_fork(CPU& cpu);
    void shutdown();
    bool poll(CPU& cpu);
    bool signal(CPU& cpu, int& signo, int& code, uint64_t& address);
    int64_t ptrace(CPU& cpu, uint64_t request, int tid, uint64_t addr, uint64_t data);
    void exited(CPU& cpu, int code, int signo = 0, bool group = false);
    void exec(CPU& cpu);
    void set_dumpable(int value);
    bool has_tracees(int selector) const;
    bool wait_polling() const;
    bool take_wait(int selector, bool stopped, bool exited, bool consume,
                   int& tid, int& status);
    void reaped(int tid);
    void wait_change();
    bool status(int tid, std::string& text) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace arm64emu
