#pragma once
#include <cstdint>
namespace arm64emu {
class CPU;
enum class NativeSimdKind : uint8_t {
    None, Select, Dup, Insert, Extract, NarrowShift, WidenShift, WidenHigh,
    NarrowFp, WidenFp, CompareFp, CompareZeroFp, CompareZeroInt, ImmediateFp,
    VariableShift, RoundFp, Convert64, ScalarAdd, ReduceAdd, Reverse64,
    MultiplyAdd, IndexedFma
};
// Compile-time classification. Runtime helpers are specialized by kind and
// execute only that operation: they never decode or step the interpreter.
NativeSimdKind classify_native_simd(uint32_t word);
using NativeSimdFn = void (*)(CPU*, uint32_t);
NativeSimdFn native_simd_helper(NativeSimdKind kind);
}
