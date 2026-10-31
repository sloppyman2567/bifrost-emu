// Yuv420.hpp — YUV_420_888 to straight ARGB for a SurfaceView's video layer.
//
// An ImageReader hands the layer three planes (Y, then U and V at half resolution, each with its own row and
// pixel stride) plus a crop rectangle. The converter scales the crop to the layer's size (nearest sample, as the
// managed side always did) and writes only the requested region of the layer, so a video far larger than the window
// costs what shows. Matrix and range come from the image's dataspace.
//
// One fixed-point definition serves every caller. A runtime-gated AVX2 kernel handles unscaled rows with planar or
// semi-planar chroma and scaled rows through a staging row; it is bit-identical to the scalar code, which remains the
// path on hosts without AVX2 (and the reference the tests compare it with).
#pragma once
#include <cstddef>
#include <cstdint>

namespace arm64emu::android {

struct Yuv420Plane {
    const uint8_t* data = nullptr;
    size_t size = 0;        // bytes readable from `data`
    int64_t row_stride = 0;
    int64_t pixel_stride = 0;
};

struct Yuv420Source {
    Yuv420Plane y, u, v;
    int64_t crop_left = 0, crop_top = 0, crop_width = 0, crop_height = 0;  // in luma samples
};

enum class YuvMatrix { Bt601 = 0, Bt709 = 1, Bt2020 = 2 };

struct Yuv420Region {
    int64_t layer_width = 0, layer_height = 0;  // the full layer the crop is scaled to
    int64_t left = 0, top = 0, width = 0, height = 0;  // the part written (inside the layer)
};

// Byte order of an output pixel in memory: Argb is the 32-bit value 0xFFRRGGBB (bytes B, G, R, A on little-endian),
// Rgba is the bytes R, G, B, A that a GL_RGBA/GL_UNSIGNED_BYTE upload reads.
enum class YuvOrder { Argb, Rgba };

struct Yuv420Options {
    YuvOrder order = YuvOrder::Argb;
    bool scalar_only = false;  // skip the SIMD kernel (tests, and the reference they compare it with)
};

// Writes region.width * region.height pixels. Returns 0, or -EINVAL when the geometry is not valid or a sample would
// lie outside a plane. No read goes past a plane's `size`, including the kernel's wide loads.
int yuv420_to_argb(const Yuv420Source& source, const Yuv420Region& region, YuvMatrix matrix, bool full_range,
                   uint32_t* out, const Yuv420Options& options = {});

}  // namespace arm64emu::android
