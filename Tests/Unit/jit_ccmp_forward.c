// FreeType's gray rasterizer keeps W0 live across a flags-only CCMP.
// Forwarding must retain that value even if a later W0 write makes its
// earlier architectural STORE_REG removable.
#include <stdint.h>
#include <stdio.h>

// Keep compare and select in one block, as in the actual rasterizer.
static unsigned char raster_ccmp(int value, int mask) {
    unsigned char pixel = 0;
    __asm__ volatile(
        "mov w0, %w[value]\n"
        "mov w3, %w[mask]\n"
        "mov w4, #255\n"
        "asr w0, w0, #9\n"
        "tst w3, w0\n"
        "cinv w0, w0, ne\n"
        "cmp w0, #255\n"
        "ccmp w3, #0, #0, gt\n"
        "csel w0, w0, w4, ge\n"
        "strb w0, [%[out]]\n"
        "mov w0, #17\n"
        :
        : [value] "r"(value), [mask] "r"(mask), [out] "r"(&pixel)
        : "x0", "x3", "x4", "cc", "memory");
    return pixel;
}

static unsigned char raster_ccmn(int value, int mask) {
    unsigned char pixel = 0;
    __asm__ volatile(
        "mov w0, %w[value]\n"
        "mov w3, %w[mask]\n"
        "mov w4, #255\n"
        "asr w0, w0, #9\n"
        "tst w3, w0\n"
        "cinv w0, w0, ne\n"
        "cmp w0, #255\n"
        "ccmn w3, #0, #0, gt\n"
        "csel w0, w0, w4, ge\n"
        "strb w0, [%[out]]\n"
        "mov w0, #17\n"
        :
        : [value] "r"(value), [mask] "r"(mask), [out] "r"(&pixel)
        : "x0", "x3", "x4", "cc", "memory");
    return pixel;
}

int main(void) {
    const int masks[] = {0, -1, 1};
    for (int v = -1024; v <= 1024; ++v) {
        for (unsigned m = 0; m < sizeof(masks)/sizeof(masks[0]); ++m) {
            int value = v;
            if (masks[m] & value) value = ~value;
            unsigned char want = (unsigned char)(value > 255 && masks[m] < 0 ? 255 : value);
            if (raster_ccmp(v * 512, masks[m]) != want ||
                raster_ccmn(v * 512, masks[m]) != want) {
                printf("FAIL raster v=%d mask=%d want=%u\n", v, masks[m], want);
                return 1;
            }
        }
    }
    puts("ALL PASS: CCMP/CCMN preserve forwarded X0");
    return 0;
}
