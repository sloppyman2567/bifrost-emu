// GLSL ES 3.x has no implicit int -> uint conversion, so Mesa rejects the array index
// `(uint_expr) * 1 + 0` that SPIRV-Cross writes for flattened uniform arrays ("could not
// implicitly convert operands to arithmetic operator"); mobile drivers accept it. This is the
// bounded source rewrite the graphics thunk retries with after a driver rejects such a shader.
#pragma once
#include <string>
#include <string_view>

namespace arm64emu::shader_fix {

// True for a `#version 300 es` / `310 es` / `320 es` source (the only ones the rewrite applies to).
bool is_es3_source(std::string_view source);

// Returns `source` with `int(...)` around the operand of every array index of the exact form
// `[E * N + M]` (N, M decimal literals, E the whole index expression), or an empty string when
// the source is not an ES 3.x shader or has no such index. `int(E)` is the identity for an int
// E and the conversion the shader needed for a uint E, so only shaders that failed to compile
// are ever changed. An index whose operand is not the whole expression (`a + b * 1 + 0`) is
// left alone: wrapping `b` would leave the sum mixed.
std::string wrap_scaled_index_operands(std::string_view source);

}  // namespace arm64emu::shader_fix
