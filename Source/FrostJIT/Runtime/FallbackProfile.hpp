#pragma once
#include <cstdint>
namespace arm64emu {
// Opt-in inventory of every interpreted PC/opcode per guest and host thread.
void record_interpreted_instruction(int guest_tid, uint64_t pc, uint32_t opcode,
                                    unsigned instruction_class);
void dump_interpreted_profile();
}
