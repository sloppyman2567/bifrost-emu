// ShaderIndexConversion: the int() rewrite glCompileShader retries an ES 3.x source with.
#include "System/Graphics/Thunks/GraphicThunk/ShaderIndexConversion.hpp"
#include <cassert>
#include <cstdio>
#include <string>

using arm64emu::shader_fix::is_es3_source;
using arm64emu::shader_fix::wrap_scaled_index_operands;

static const std::string kHeader = "#version 300 es\nprecision highp float;\n";

int main() {
    // The versions the rewrite applies to, and the ones it must not touch.
    assert(is_es3_source("#version 300 es\nvoid main(){}\n"));
    assert(is_es3_source("#version 310 es\n"));
    assert(is_es3_source("#version 320 es\r\nvoid main(){}\n"));
    assert(is_es3_source("  \n#version  300   es  \n"));
    assert(!is_es3_source("#version 100\nvoid main(){}\n"));
    assert(!is_es3_source("#version 330 core\nvoid main(){}\n"));
    assert(!is_es3_source("#version 300\nvoid main(){}\n"));
    assert(!is_es3_source("void main(){}\n"));
    assert(wrap_scaled_index_operands("#version 330 core\nx = A[(k & 7u) * 1 + 0];\n").empty());

    // Both operand shapes of a failing Roblox shader, and an identifier operand.
    assert(wrap_scaled_index_operands(kHeader + "x = CB3[(_500 & 63u) * 1 + 0].xyz;\n") ==
           kHeader + "x = CB3[int((_500 & 63u)) * 1 + 0].xyz;\n");
    assert(wrap_scaled_index_operands(kHeader + "x = CB12[((uint(POSITION.w) >> 8u) & 255u) * 1 + 0];\n") ==
           kHeader + "x = CB12[int(((uint(POSITION.w) >> 8u) & 255u)) * 1 + 0];\n");
    assert(wrap_scaled_index_operands(kHeader + "x = CB1[_7 * 3 + 2];\n") == kHeader + "x = CB1[int(_7) * 3 + 2];\n");
    // A call, a field chain and an array element are one operand.
    assert(wrap_scaled_index_operands(kHeader + "x = A[uint(v.w) * 1 + 0];\n") == kHeader + "x = A[int(uint(v.w)) * 1 + 0];\n");
    assert(wrap_scaled_index_operands(kHeader + "x = A[s.idx.x * 2 + 1];\n") == kHeader + "x = A[int(s.idx.x) * 2 + 1];\n");
    assert(wrap_scaled_index_operands(kHeader + "x = A[B[i] * 1 + 0];\n") == kHeader + "x = A[int(B[i]) * 1 + 0];\n");

    // Several indices, and an index nested in an operand.
    assert(wrap_scaled_index_operands(kHeader + "a = A[u * 1 + 0]; b = A[v * 1 + 1];\n") ==
           kHeader + "a = A[int(u) * 1 + 0]; b = A[int(v) * 1 + 1];\n");
    assert(wrap_scaled_index_operands(kHeader + "x = A[(B[u * 1 + 0] & 3u) * 1 + 0];\n") ==
           kHeader + "x = A[int((B[int(u) * 1 + 0] & 3u)) * 1 + 0];\n");

    // Left alone: the operand is not the whole index (the sum would stay mixed), an unscaled index,
    // a non-literal scale, an unbalanced operand, and a source with no such index.
    assert(wrap_scaled_index_operands(kHeader + "x = A[a + b * 1 + 0];\n").empty());
    assert(wrap_scaled_index_operands(kHeader + "x = A[u];\n").empty());
    assert(wrap_scaled_index_operands(kHeader + "x = A[u * k + 0];\n").empty());
    assert(wrap_scaled_index_operands(kHeader + "x = A[u) * 1 + 0];\n").empty());
    assert(wrap_scaled_index_operands(kHeader + "void main() { gl_Position = vec4(1.0); }\n").empty());
    std::puts("shader_index_conversion: ALL PASS");
    return 0;
}
