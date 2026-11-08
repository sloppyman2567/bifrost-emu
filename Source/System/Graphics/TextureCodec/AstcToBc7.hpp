// AstcToBc7.hpp — transcodes an ASTC LDR image to BC7.
//
// Desktop GPUs without ASTC do have BC7, which is the same 8 bits per texel as ASTC 4x4 and keeps far more detail than
// an 8-bit RGBA fallback at a quarter of the size. The image is decoded and re-encoded in strips so the temporary
// memory is a few hundred kilobytes per thread, whatever the texture size.
#pragma once
#include "System/Graphics/TextureCodec/Bc7Encode.hpp"
#include <cstddef>
#include <cstdint>

namespace arm64emu::texcodec {

struct AstcRegion {
    const uint8_t* blocks;  // ASTC blocks, raster order
    size_t row_pitch;       // bytes from one row of blocks to the next (at least ceil(width / block_w) * 16)
    int block_w, block_h;   // footprint
    bool srgb;
    int width, height;      // texels
};

// Bytes a BC7 image of `width` x `height` texels takes with rows packed.
size_t bc7_image_bytes(int width, int height);

// Writes the BC7 blocks for the region to `out`, rows packed (ceil(width / 4) * 16 bytes). `threads` of 0 or 1 runs on
// the calling thread. Returns false for an invalid footprint or size; `out` is then untouched.
bool transcode_astc_to_bc7(const AstcRegion& src, const Bc7Options& options, unsigned threads, uint8_t* out);

}  // namespace arm64emu::texcodec
