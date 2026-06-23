// ir/ir_builder.cpp — vreg allocator state + reset.
//
// The emit/load_imm/load_arm_reg helpers are inline in ir.h. Only the
// thread-local allocator instance and its reset function live here.
#include "ir/ir.h"

namespace arm64emu {

thread_local VregAlloc g_alloc;

void ir_reset_vreg_alloc() { g_alloc.reset(); }

} // namespace arm64emu
