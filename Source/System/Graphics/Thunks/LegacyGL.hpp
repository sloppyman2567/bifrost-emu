#pragma once
#include "System/Core/CPU.hpp"
#include "System/Memory/Memory.hpp"
#include "Generated/ThunkTable.hpp"
namespace arm64emu {
// Typed legacy GL setters and evaluator/program buffers. No retained pointers.
void dispatch_legacy_gl(Memory& mem, CPU& cpu, const thunk::Spec& spec, void* host_fn);
}
