// SampleProfile.hpp — BIFROST_PROF: where host time goes, by thread and by part of the translated code.
//
// The SIGPROF handler (EmulatorBridge.cpp) hands every interrupted host pc inside a thread's code buffer to
// prof_record_sample(). Each translated block registers where its parts are (entry: loading the canonical register
// layout; body: the translated instructions; exit: publishing state; cold: recovery and exits), and the report puts each
// sample into one of them. Anything in a code buffer but outside a registered block is the shared stubs (frame exit,
// cold-path stubs) or a block loaded from the persistent cache, which is not registered.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace arm64emu {

// Absolute host addresses of one translated block (a Tier-1 block or a Tier-2 region).
struct ProfBlock {
    uint64_t begin, body, exit, cold, end;
    uint64_t guest_pc;
};

// What an IR operation's emitted code does, for splitting the body's time. A memory operation includes its guest-address
// guard, and any operation includes the spill and reload code the allocator put in it (`spill` marks those).
enum ProfOp : uint8_t { PROF_STATE, PROF_MEMORY, PROF_ALU, PROF_FLAGS, PROF_BRANCH, PROF_CALL, PROF_FPSIMD, PROF_OTHER,
                        PROF_OP_KINDS };

// The code of one IR operation inside a block's body.
struct ProfRange {
    uint64_t begin, end;
    uint8_t op;
    bool spill;
};

struct ProfSectionCounts {
    uint64_t entry = 0, body = 0, exit = 0, cold = 0;
    uint64_t shared = 0;  // inside a code buffer, outside any registered block
    uint64_t total() const { return entry + body + exit + cold + shared; }
    ProfSectionCounts& operator+=(const ProfSectionCounts& o) {
        entry += o.entry; body += o.body; exit += o.exit; cold += o.cold; shared += o.shared;
        return *this;
    }
};

struct ProfThreadReport {
    int host_tid;
    bool exited;
    ProfSectionCounts counts;
};

struct ProfBlockReport {
    uint64_t guest_pc;
    ProfSectionCounts counts;
};

struct ProfReport {
    ProfSectionCounts total;
    uint64_t lost = 0;  // samples that never reached a ring (no free slot) or were overwritten before they were read
    uint64_t body_op[PROF_OP_KINDS] = {};  // the body samples by what the operation under them does (PROF_OTHER: no range)
    uint64_t body_spill = 0;                // body samples inside an operation whose code contains spills or reloads
    uint64_t ranges_dropped = 0;            // operation ranges not kept because the table was full
    std::vector<ProfThreadReport> threads;
    std::vector<ProfBlockReport> hottest;  // most samples first
};

// Called while translating, only when BIFROST_PROF is set: the block, then the operations in its body.
void prof_note_block(const ProfBlock& block);
void prof_note_ranges(const ProfRange* ranges, size_t count);
// A code buffer is about to be unmapped: resolve what was sampled in it, then forget its blocks.
void prof_release_code(const uint8_t* base, size_t size);
// Gives the calling thread a ring for its samples (once per thread).
void prof_claim_thread();
// Signal handler: the interrupted pc lies inside the calling thread's code buffer. Async-signal-safe.
void prof_record_sample(uint64_t rip);
// Resolves every sample not yet resolved, on all threads, and returns what has been seen since the process began.
ProfReport prof_collect(size_t top_blocks);
// One summary line, a line per thread, and (list_blocks) the hottest blocks.
void prof_print(std::FILE* out, const ProfReport& report, bool list_blocks);

}  // namespace arm64emu
