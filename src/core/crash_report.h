// core/crash_report.h — one shared guest-crash reporter.
//
// replaces per-app probes with a single short + detailed dump.
#pragma once
#include <cstdint>

namespace arm64emu {
class CPU;
class Memory;

// always prints one short [crash] line. prints regs + stack +
// fp chain only when a detail flag is on (crash_dump / dbg_guard /
// trace_crash, or BIFROST_TRACE=1). never throws. pass inst
// as nullptr when no faulting instruction word is known.
// mod_name / mod_offset are optional (nullptr / 0 = unknown).
void report_crash(const CPU& cpu, const Memory& mem, const char* reason,
                  uint64_t fault_addr, const uint32_t* inst,
                  const char* mod_name = nullptr, uint64_t mod_offset = 0);
}  // namespace arm64emu
