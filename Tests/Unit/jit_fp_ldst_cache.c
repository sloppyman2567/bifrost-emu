// jit_fp_ldst_cache.c — scalar FP loads and stores (LDR/STR B/H/S/D, LDP/STP S/D) are cache-aware, so
// blocks that hold them keep their FP values in XMM registers. Every section hashes the architectural
// result, including the upper 64 bits that a scalar write must clear, and compares it with the value
// produced by QEMU's user-mode emulator (the table below). Fault behaviour of these accesses is
// covered by jit_host_fault.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t h;
static void mix(uint64_t x) { h = (h ^ x) * 1099511628211ull; h ^= h >> 31; }
static void mix_q(const void* q) { uint64_t w[2]; memcpy(w, q, 16); mix(w[0]); mix(w[1]); }

typedef struct { float px, py, pz, vx, vy, vz; int32_t hp; int32_t flags; } Ent;

__attribute__((noipa)) static double dsum(const double* a, const double* b, double* o, int n) {
    double s = 0;
    for (int i = 0; i < n; ++i) { double x = a[i] * b[i] - a[i] / 3.0; o[i] = x; s += x; }
    return s;
}
__attribute__((noipa)) static float fnorm(const float* a, float* o, int n, float g) {
    float s = 0;
    for (int i = 0; i < n; ++i) { float x = a[i] * g + 0.25f; o[i] = x > 1000.f ? 1000.f : x; s += o[i] * o[i]; }
    return s;
}
__attribute__((noipa)) static void ent_step(Ent* e, int n, float dt) {
    for (int i = 0; i < n; ++i) {
        e[i].px += e[i].vx * dt; e[i].py += e[i].vy * dt; e[i].pz += e[i].vz * dt;
        if (e[i].px > 100.f) { e[i].px = 100.f; e[i].vx = -e[i].vx; }
        if (e[i].flags & 1) e[i].hp -= 1;
    }
}
__attribute__((noipa)) static double conv(const int32_t* a, double* o, int n) {
    double s = 0;
    for (int i = 0; i < n; ++i) { o[i] = (double)a[i] * 0.5; s += o[i]; }
    return s;
}
__attribute__((noipa)) static void minmax(const float* a, float* lo, float* hi, int n) {
    float l = a[0], u = a[0];
    for (int i = 1; i < n; ++i) { if (a[i] < l) l = a[i]; if (a[i] > u) u = a[i]; }
    *lo = l; *hi = u;
}

int main(void) {
    static double da[64], db[64], dout[64]; static float fa[64], fout[64]; static Ent ents[16];
    static int32_t ia[64]; static uint8_t buf[256] __attribute__((aligned(16)));
    for (int i = 0; i < 64; ++i) { da[i] = 1.0 + 0.37 * i; db[i] = 2.0 - 0.11 * i; fa[i] = 0.5f + 7.25f * i; ia[i] = i * 977 - 12345; }
    for (int i = 0; i < 16; ++i) { ents[i].px = 90.f + i; ents[i].vx = 1.5f; ents[i].vy = 0.25f; ents[i].vz = -0.5f; ents[i].hp = 9; ents[i].flags = i; }
    uint64_t got[16]; int ng = 0;

    // 1. compiler-generated loops: D and S loads/stores, conversions, compares, struct fields.
    h = 1469598103934665603ull;
    for (int r = 0; r < 40; ++r) {
        double s = dsum(da, db, dout, 64); uint64_t b; memcpy(&b, &s, 8); mix(b);
        for (int i = 0; i < 64; i += 9) { memcpy(&b, &dout[i], 8); mix(b); }
        float f = fnorm(fa, fout, 64, 1.0f + 0.01f * r); uint32_t fb; memcpy(&fb, &f, 4); mix(fb);
        for (int i = 0; i < 64; i += 7) { memcpy(&fb, &fout[i], 4); mix(fb); }
        ent_step(ents, 16, 0.016f);
        for (int i = 0; i < 16; ++i) { uint64_t w[4]; memcpy(w, &ents[i], 32); for (int k = 0; k < 4; ++k) mix(w[k]); }
        double c = conv(ia, dout, 64); memcpy(&b, &c, 8); mix(b);
        float lo, hi; minmax(fa, &lo, &hi, 64); memcpy(&fb, &lo, 4); mix(fb); memcpy(&fb, &hi, 4); mix(fb);
    }
    got[ng++] = h;

    // 2. one asm block per form, with the destination preloaded with ones so a missed upper-lane clear shows.
    h = 1469598103934665603ull;
    uint8_t src[64] __attribute__((aligned(16)));
    for (int i = 0; i < 64; ++i) src[i] = (uint8_t)(i * 37 + 11);
    for (int r = 0; r < 4; ++r) {
        uint8_t q[16 * 8] __attribute__((aligned(16)));
        memset(q, 0xEE, sizeof q);
        __asm__ volatile(
            "movi v0.16b, #0xff\n movi v1.16b, #0xff\n movi v2.16b, #0xff\n movi v3.16b, #0xff\n"
            "movi v4.16b, #0xff\n movi v5.16b, #0xff\n movi v6.16b, #0xff\n movi v7.16b, #0xff\n"
            "ldr b0, [%1, #1]\n ldr h1, [%1, #2]\n ldr s2, [%1, #4]\n ldr d3, [%1, #8]\n"
            "ldp s4, s5, [%1, #16]\n ldp d6, d7, [%1, #24]\n"
            "str q0, [%0]\n str q1, [%0, #16]\n str q2, [%0, #32]\n str q3, [%0, #48]\n"
            "str q4, [%0, #64]\n str q5, [%0, #80]\n str q6, [%0, #96]\n str q7, [%0, #112]\n"
            : : "r"(q), "r"(src + r)
            : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "memory");
        for (int i = 0; i < 8; ++i) mix_q(q + 16 * i);
        uint8_t o[64]; memset(o, 0x5A, sizeof o);
        // stores of every width, and the post/pre-index pair forms; scalar ops in between.
        uint8_t* p = o;
        __asm__ volatile(
            "ldr d0, [%1]\n ldr d1, [%1, #8]\n fadd d2, d0, d1\n fmul d3, d0, d1\n"
            "str b2, [%0]\n str h2, [%0, #2]\n str s2, [%0, #4]\n str d3, [%0, #8]\n"
            "stp s2, s3, [%0, #16]\n stp d2, d3, [%0, #24]\n"
            "stp d3, d2, [%0, #40]!\n str d0, [%0, #8]!\n str s1, [%0], #4\n"
            : "+r"(p) : "r"(src + 8 + r)
            : "v0", "v1", "v2", "v3", "memory");
        for (int i = 0; i < 64; ++i) mix(o[i]);
        mix((uint64_t)(p - o));
    }
    got[ng++] = h;

    // 3. more live FP registers than the cache holds: loads and stores of d0..d31 around arithmetic.
    h = 1469598103934665603ull;
    for (int r = 0; r < 8; ++r) {
        double in[32], out[32];
        for (int i = 0; i < 32; ++i) in[i] = 1.5 + i * 0.75 + r;
        __asm__ volatile(
            "ldp d0, d1, [%1]\n ldp d2, d3, [%1, #16]\n ldp d4, d5, [%1, #32]\n ldp d6, d7, [%1, #48]\n"
            "ldp d8, d9, [%1, #64]\n ldp d10, d11, [%1, #80]\n ldp d12, d13, [%1, #96]\n ldp d14, d15, [%1, #112]\n"
            "ldp d16, d17, [%1, #128]\n ldp d18, d19, [%1, #144]\n ldp d20, d21, [%1, #160]\n ldp d22, d23, [%1, #176]\n"
            "ldp d24, d25, [%1, #192]\n ldp d26, d27, [%1, #208]\n ldp d28, d29, [%1, #224]\n ldp d30, d31, [%1, #240]\n"
            "fadd d0, d0, d31\n fmul d1, d1, d30\n fsub d2, d2, d29\n fadd d3, d3, d28\n"
            "fmul d16, d16, d15\n fadd d17, d17, d14\n fsub d18, d18, d13\n fadd d19, d19, d12\n"
            "fmul d31, d31, d0\n fadd d30, d30, d1\n fsub d29, d29, d2\n fadd d28, d28, d3\n"
            "stp d0, d1, [%0]\n stp d2, d3, [%0, #16]\n stp d4, d5, [%0, #32]\n stp d6, d7, [%0, #48]\n"
            "stp d8, d9, [%0, #64]\n stp d10, d11, [%0, #80]\n stp d12, d13, [%0, #96]\n stp d14, d15, [%0, #112]\n"
            "stp d16, d17, [%0, #128]\n stp d18, d19, [%0, #144]\n stp d20, d21, [%0, #160]\n stp d22, d23, [%0, #176]\n"
            "stp d24, d25, [%0, #192]\n stp d26, d27, [%0, #208]\n stp d28, d29, [%0, #224]\n stp d30, d31, [%0, #240]\n"
            : : "r"(out), "r"(in)
            : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22",
              "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "memory");
        for (int i = 0; i < 32; ++i) { uint64_t b; memcpy(&b, &out[i], 8); mix(b); }
    }
    got[ng++] = h;

    static const uint64_t want[] = {0x7c0e4962a5131518ull, 0xb226370b286e2e64ull, 0x60d3c0bb6c8a9752ull};
    int fails = 0;
    for (int i = 0; i < ng; ++i)
        if (got[i] != want[i]) { ++fails; printf("NG section %d: got %016llx want %016llx\n", i + 1, (unsigned long long)got[i], (unsigned long long)want[i]); }
    printf("jit_fp_ldst_cache: %s\n", fails ? "FAILED" : "ALL PASS");
    return fails != 0;
}
