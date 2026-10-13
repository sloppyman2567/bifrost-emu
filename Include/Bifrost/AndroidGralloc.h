#pragma once

#include <stdint.h>
#include <stddef.h>

// Versioned transport shared by the broker's allocator and guest mapper.
// One fd contains metadata, linear pixels, then the caller's reserved region.
// Native handles contain one fd followed by these descriptor integers.
enum { BF_GRALLOC_MAGIC = 0x42464735, BF_GRALLOC_INTS = 12, BF_GRALLOC_OFFSET = 4096 };
enum { BF_MAGIC, BF_WIDTH, BF_HEIGHT, BF_STRIDE, BF_REQUESTED, BF_FORMAT,
       BF_USAGE_LO, BF_USAGE_HI, BF_SIZE_LO, BF_SIZE_HI, BF_RESERVED_LO, BF_RESERVED_HI };
struct bf_gralloc_header {
    int32_t info[BF_GRALLOC_INTS];
    char name[128];
    // Atomic accesses by the guest mapper. Bounded retries return NO_RESOURCES
    // if a writer is interrupted, rather than waiting forever on a dead app.
    uint32_t sequences[6];
    uint32_t lengths[6]; // DATASPACE, BLEND_MODE, SMPTE2086, CTA861_3, SMPTE2094_{40,10}
    unsigned char metadata[6][512];
};
#ifdef __cplusplus
static_assert(sizeof(struct bf_gralloc_header) <= BF_GRALLOC_OFFSET, "gralloc metadata page");
#else
_Static_assert(sizeof(struct bf_gralloc_header) <= BF_GRALLOC_OFFSET, "gralloc metadata page");
#endif

static inline uint64_t bf_gralloc_u64(const int32_t *info, int field) {
    return (uint32_t)info[field] | ((uint64_t)(uint32_t)info[field + 1] << 32);
}
static inline void bf_gralloc_put64(int32_t *info, int field, uint64_t value) {
    info[field] = value;
    info[field + 1] = value >> 32;
}
static inline uint64_t bf_gralloc_reserved_offset(const int32_t *info) {
    // IMapper requires the caller's reserved region to be word-aligned even
    // for packed RGB888 pixels whose byte count is not a multiple of a word.
    return BF_GRALLOC_OFFSET + ((bf_gralloc_u64(info, BF_SIZE_LO) + 7) & ~UINT64_C(7));
}
static inline uint64_t bf_gralloc_file_size(const int32_t *info) {
    return bf_gralloc_reserved_offset(info) + bf_gralloc_u64(info, BF_RESERVED_LO);
}
static inline int bf_gralloc_bpp(int format) {
    switch (format) {
    case 1: case 2: case 5: case 43: return 4; // RGBA, RGBX, BGRA, 1010102
    case 3: return 3;
    case 4: return 2;
    case 22: return 8; // FP16
    case 56: case 33: return 1; // R8, BLOB
    default: return 0;
    }
}
// A CPU-backed, one-layer allocation. GPU import is handled separately by EGL.
// Reject protected, tiled, cube/mipmap, front-buffer and vendor-only requests.
static inline int bf_gralloc_layout(int width, int height, int layers, int format,
        uint64_t usage, uint64_t reserved, int32_t *info) {
    // libui's Gralloc5::getValidUsageBits explicitly retains the removed
    // HW_2D/EXTERNAL_DISP hints (bits 10 and 13) for existing callers.
    const uint64_t allowed = 0xff | 0x100 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x2000 | 0x10000 | 0x400000;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || layers != 1
            || reserved > 1024 * 1024 || (usage & ~allowed)
            || ((usage & 15) != 0 && (usage & 15) != 2 && (usage & 15) != 3)
            || ((usage & 240) != 0 && (usage & 240) != 32 && (usage & 240) != 48)) return 0;
    int actual = format == 34 ? 1 : format; // IMPLEMENTATION_DEFINED -> linear RGBA
    int bpp = bf_gralloc_bpp(actual), stride = width;
    uint64_t size;
    if (actual == 35 || actual == 0x32315659) { // flexible YUV420 and YV12: planar Y,V,U
        if ((width | height) & 1) return 0;
        stride = (width + 15) & ~15;
        int chroma_stride = ((stride / 2) + 15) & ~15;
        size = (uint64_t)stride * height + (uint64_t)chroma_stride * height;
    } else {
        if (!bpp || (actual == 33 && height != 1)) return 0;
        size = (uint64_t)stride * height * bpp;
    }
    if (size > 256 * 1024 * 1024) return 0;
    info[BF_MAGIC] = BF_GRALLOC_MAGIC;
    info[BF_WIDTH] = width; info[BF_HEIGHT] = height; info[BF_STRIDE] = stride;
    info[BF_REQUESTED] = format; info[BF_FORMAT] = actual;
    bf_gralloc_put64(info, BF_USAGE_LO, usage);
    bf_gralloc_put64(info, BF_SIZE_LO, size);
    bf_gralloc_put64(info, BF_RESERVED_LO, reserved);
    return 1;
}
