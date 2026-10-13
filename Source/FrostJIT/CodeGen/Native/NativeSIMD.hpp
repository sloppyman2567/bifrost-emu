#pragma once
#include <cstdint>
// NativeSimdKind and classify_native_simd (compile-time classification) are
// generated from Tools/CodeGen/native_simd.txt.
#include "Generated/NativeSimdTable.hpp"
namespace arm64emu {
class CPU;
// Runtime helpers are specialized by kind and execute only that operation:
// they never decode or step the interpreter.
using NativeSimdFn = void (*)(CPU*, uint32_t);
NativeSimdFn native_simd_helper(NativeSimdKind kind);
}
