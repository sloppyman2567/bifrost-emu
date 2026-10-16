#pragma once
#include <cstdint>
namespace arm64emu {
enum class InterpretedProfileReason { General, FpSpecial };
// A scope covers one interpreter step, not a guest thread's lifetime.
// Nested steps restore their caller's reason; no state is shared by threads.
extern thread_local InterpretedProfileReason interpreted_profile_reason;
class InterpretedProfileScope {
    InterpretedProfileReason previous_ = interpreted_profile_reason;
public:
    explicit InterpretedProfileScope(InterpretedProfileReason reason) {
        interpreted_profile_reason = reason;
    }
    ~InterpretedProfileScope() { interpreted_profile_reason = previous_; }
    InterpretedProfileScope(const InterpretedProfileScope&) = delete;
    InterpretedProfileScope& operator=(const InterpretedProfileScope&) = delete;
};
// Opt-in inventory of every interpreted PC/opcode per guest and host thread.
void record_interpreted_instruction(int guest_tid, uint64_t pc, uint32_t opcode,
                                    unsigned instruction_class);
void dump_interpreted_profile();
}
