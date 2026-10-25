// The graphics thunk this thread is inside, for the host-fault report. A host crash in a driver
// or libc called from a thunk otherwise names only a libc PC, and finding the GL call took a core
// dump. The pointer is the symbol's name (a literal in the generated table, else the registered
// entry's, which outlives dispatch); the fault handler only reads it, so nothing here allocates.
#pragma once

namespace arm64emu::thunk_call {

inline thread_local const char* in_flight = nullptr;

// Nests: a thunk may run guest code (a callback) that calls another thunk.
struct Scope {
    const char* previous;
    explicit Scope(const char* name) : previous(in_flight) { in_flight = name; }
    ~Scope() { in_flight = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}  // namespace arm64emu::thunk_call
