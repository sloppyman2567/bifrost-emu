// md5_scalar_test.c — minimal scalar MD5 to find which step is broken.
//
// This mirrors what toybox does: scalar C with ror() inline asm.
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t ror(uint32_t v, unsigned n) {
    uint32_t r;
    __asm__ volatile ("ror %w0, %w1, %w2" : "=r"(r) : "r"(v), "r"(n));
    return r;
}

// MD5 K table (sin-based constants)
static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

static const uint32_t S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

#define LEFTROTATE(x, c) ror((x), (32 - (c)))

static void md5(const uint8_t *msg, size_t len, uint32_t out[4]) {
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;

    // padding
    size_t new_len = ((len + 8) / 64 + 1) * 64;
    uint8_t buf[256] = {0};
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[new_len - 8 + i] = (bits >> (8*i)) & 0xff;

    for (size_t off = 0; off < new_len; off += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++) {
            M[i] = (uint32_t)buf[off + i*4]
                 | ((uint32_t)buf[off + i*4 + 1] << 8)
                 | ((uint32_t)buf[off + i*4 + 2] << 16)
                 | ((uint32_t)buf[off + i*4 + 3] << 24);
        }
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F; int g;
            if (i < 16)      { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;         g = (3*i + 5) % 16; }
            else             { F = C ^ (B | ~D);      g = (7*i) % 16; }
            uint32_t t = A + F + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + LEFTROTATE(t, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    out[0] = a0; out[1] = b0; out[2] = c0; out[3] = d0;
}

int main(void) {
    const char *tests[] = {"", "a", "abc", "hello", "hello world"};
    for (int t = 0; t < 5; t++) {
        const char *s = tests[t];
        size_t len = strlen(s);
        uint32_t out[4];
        md5((const uint8_t*)s, len, out);
        // MD5 outputs in little-endian byte order
        printf("md5(\"%s\") = ", s);
        for (int i = 0; i < 4; i++) {
            printf("%02x%02x%02x%02x", out[i] & 0xff, (out[i]>>8) & 0xff,
                   (out[i]>>16) & 0xff, (out[i]>>24) & 0xff);
        }
        printf("\n");
    }
    return 0;
}
