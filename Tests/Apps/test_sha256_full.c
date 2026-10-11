// End-to-end SHA-1/SHA-256 using guest crypto instructions for the schedule
// and compression. Digest literals were independently checked with hashlib.
// Covers empty input, abc, padding across blocks, and a longer message.
#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint32_t k256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void digest(const unsigned char *message, size_t length, int sha256, char *hex) {
    unsigned char padded[1088] = {0};
    memcpy(padded, message, length);
    padded[length] = 0x80;
    size_t total = (length + 9 + 63) & ~(size_t)63;
    uint64_t bits = length * 8;
    for (int i = 0; i < 8; ++i) padded[total - 1 - i] = bits >> (8 * i);
    uint32_t state[8];
    const uint32_t initial256[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                  0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const uint32_t initial1[8] = {0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0,0,0,0};
    memcpy(state, sha256 ? initial256 : initial1, sizeof(state));
    for (size_t block = 0; block < total; block += 64) {
        uint32_t words[16];
        for (int i = 0; i < 16; ++i) {
            const unsigned char *p = padded + block + i * 4;
            words[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                       ((uint32_t)p[2] << 8) | p[3];
        }
        uint32x4_t w[4];
        for (int i = 0; i < 4; ++i) w[i] = vld1q_u32(words + 4 * i);
        uint32x4_t a = vld1q_u32(state), b = vld1q_u32(state + 4);
        uint32_t e = state[4];
        int groups = sha256 ? 16 : 20;
        for (int group = 0; group < groups; ++group) {
            int slot = group % 4;
            if (group >= 4) {
                if (sha256)
                    w[slot] = vsha256su1q_u32(vsha256su0q_u32(w[slot], w[(slot+1)%4]),
                                             w[(slot+2)%4], w[(slot+3)%4]);
                else
                    w[slot] = vsha1su1q_u32(vsha1su0q_u32(w[slot], w[(slot+1)%4], w[(slot+2)%4]),
                                           w[(slot+3)%4]);
            }
            if (sha256) {
                uint32x4_t wk = vaddq_u32(w[slot], vld1q_u32(k256 + group * 4));
                uint32x4_t old_a = a;
                a = vsha256hq_u32(a, b, wk);
                b = vsha256h2q_u32(b, old_a, wk);
            } else {
                uint32_t k = group < 5 ? 0x5a827999 : group < 10 ? 0x6ed9eba1 :
                             group < 15 ? 0x8f1bbcdc : 0xca62c1d6;
                uint32x4_t wk = vaddq_u32(w[slot], vdupq_n_u32(k));
                uint32_t next_e = vsha1h_u32(vgetq_lane_u32(a, 0));
                if (group < 5) a = vsha1cq_u32(a, e, wk);
                else if (group < 10 || group >= 15) a = vsha1pq_u32(a, e, wk);
                else a = vsha1mq_u32(a, e, wk);
                e = next_e;
            }
        }
        vst1q_u32(state, vaddq_u32(vld1q_u32(state), a));
        if (sha256) vst1q_u32(state + 4, vaddq_u32(vld1q_u32(state + 4), b));
        else state[4] += e;
    }
    for (int i = 0; i < (sha256 ? 8 : 5); ++i) sprintf(hex + 8 * i, "%08x", state[i]);
}

int main(void) {
    static const char *expected[4][2] = {
        {"da39a3ee5e6b4b0d3255bfef95601890afd80709", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"a9993e364706816aba3e25717850c26c9cd0d89d", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"84983e441c3bd26ebaae4aa1f95129e5e54670f1", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"291e9a6c66994949b57ba5e650361e98fc36b1ba", "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"}
    };
    unsigned char long_message[1000];
    memset(long_message, 'a', sizeof(long_message));
    const unsigned char *messages[4] = {(const unsigned char *)"", (const unsigned char *)"abc",
        (const unsigned char *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", long_message};
    const size_t lengths[4] = {0, 3, 56, 1000};
    int passed = 0;
    for (int message = 0; message < 4; ++message) {
        for (int sha256 = 0; sha256 < 2; ++sha256) {
            char hex[65];
            digest(messages[message], lengths[message], sha256, hex);
            if (!strcmp(hex, expected[message][sha256])) ++passed;
            else printf("FAIL SHA%d length=%zu: %s\n", sha256 ? 256 : 1, lengths[message], hex);
        }
    }
    printf("%d/8 checks passed\n", passed);
    return passed == 8 ? 0 : 1;
}
