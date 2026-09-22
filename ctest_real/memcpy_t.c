// memcpy/memset/memmove differential: exercises glibc string routines
// across sizes/alignments, prints adler. Any JIT-vs-interp diff = bug.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t adler(const uint8_t *d, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

int main(void) {
    static uint8_t src[8192], dst[8192];
    for (size_t i = 0; i < sizeof(src); i++) src[i] = (uint8_t)(i * 31 + 7);
    uint32_t acc = 0;
    for (size_t len = 0; len < 300; len++) {
        for (size_t off = 0; off < 16; off++) {
            memset(dst, 0xAA, sizeof(dst));
            memcpy(dst + off, src + (off * 3) % 16, len);
            acc ^= adler(dst, sizeof(dst)) + (uint32_t)len * 0x9e3779b9u + (uint32_t)off;
            memset(dst + off, (int)(len & 0xFF), len);
            acc ^= adler(dst, sizeof(dst)) + (uint32_t)len + ((uint32_t)off << 8);
            memmove(dst + off + 1, dst + off, len);
            acc ^= adler(dst, sizeof(dst)) + (uint32_t)len * 3u + (uint32_t)off;
        }
    }
    /* unaligned larger copies */
    for (size_t len = 300; len < 5000; len += 137) {
        size_t off = len % 64;
        memcpy(dst + off, src + (off * 7) % 64, len);
        acc ^= adler(dst, sizeof(dst)) + (uint32_t)len;
    }
    printf("memcpy acc=0x%08x %s\n", acc, acc ? "X" : "X");
    return 0;
}
