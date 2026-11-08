// AstcDecode.hpp — decoder for ASTC LDR 2D blocks (Khronos Data Format Specification, "ASTC").
//
// Android apps (Unity, Unreal) upload ASTC when the device reports textureCompressionASTC_LDR; most desktop GPUs have
// no ASTC. This decodes one block to RGBA8 so it can be re-encoded to a format the host does have (AstcToBc7.hpp).
#pragma once
#include <cstdint>

namespace arm64emu::texcodec {

constexpr int ASTC_MAX_BLOCK_DIM = 12;

// True for the 14 footprints of the LDR profile (4x4 ... 12x12), the ones VK_FORMAT_ASTC_*_BLOCK names.
bool astc_footprint_valid(int block_w, int block_h);

// Decodes one 16-byte block of a `block_w` x `block_h` footprint to `rgba` (row-major, block_w*block_h*4 bytes).
// `srgb` selects the sRGB operation mode (8-bit endpoint precision; the result stays sRGB-encoded, as the sampler
// expects for an sRGB format). Returns false for an illegal encoding or an HDR-only feature: `rgba` then holds the
// error colour (opaque magenta) for the affected texels, which is what the specification asks of an LDR decoder.
bool astc_decode_block(const uint8_t* block, int block_w, int block_h, bool srgb, uint8_t* rgba);

}  // namespace arm64emu::texcodec
