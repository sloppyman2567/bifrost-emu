// jit_native_simd_alias.c — inline native SIMD kinds whose Vd aliases a
// source: BSL/BIT/BIF and MLA read Vd, DUP/ADDV/FRINT overwrite their input.
// The generated native SIMD test pins Vd, Vn and Vm to distinct registers.
// Every source must be read before Vd is written, as in the helper.
// The expected FNV-1a hash of all 29 results comes from qemu-aarch64.
#include <stdint.h>
#include <stdio.h>

static uint64_t h = 1469598103934665603ull;
static void mix(const uint8_t* b) {
    for (int i = 0; i < 16; ++i) h = (h ^ b[i]) * 1099511628211ull;
}
#define T(insn)                                                                 \
    do {                                                                        \
        uint8_t v[16];                                                          \
        __asm__ volatile("ldr q0, [%1]\n ldr q1, [%2]\n" insn "\n str q0, [%0]" \
                         :: "r"(v), "r"(a), "r"(b) : "v0", "v1", "memory");     \
        mix(v);                                                                 \
    } while (0)

int main(void) {
    uint8_t a[16], b[16];
    for (int i = 0; i < 16; ++i) {
        a[i] = (uint8_t)(i * 37 + 11);
        b[i] = (uint8_t)(0xF0 ^ i * 13);
    }
    T("bsl v0.16b, v0.16b, v1.16b"); T("bsl v0.16b, v1.16b, v0.16b"); T("bsl v0.8b, v0.8b, v0.8b");
    T("bit v0.16b, v0.16b, v1.16b"); T("bit v0.16b, v1.16b, v0.16b"); T("bif v0.16b, v1.16b, v0.16b");
    T("bif v0.8b, v0.8b, v1.8b");
    T("mla v0.16b, v0.16b, v1.16b"); T("mla v0.8h, v1.8h, v0.8h"); T("mla v0.4s, v0.4s, v0.4s");
    T("mla v0.8b, v1.8b, v1.8b"); T("mla v0.4h, v0.4h, v1.4h"); T("mla v0.2s, v1.2s, v0.2s");
    T("dup v0.16b, v0.b[13]"); T("dup v0.8h, v0.h[5]"); T("dup v0.4s, v0.s[3]"); T("dup v0.2d, v0.d[1]");
    T("dup v0.8b, v1.b[15]"); T("dup v0.4h, v0.h[7]"); T("dup v0.2s, v0.s[1]");
    T("addv b0, v0.16b"); T("addv h0, v0.8h"); T("addv s0, v0.4s"); T("addv b0, v0.8b");
    T("addv h0, v0.4h");
    T("frintn v0.4s, v0.4s"); T("frintm v0.2d, v0.2d"); T("frintp v0.2s, v1.2s");
    T("frintz v0.4s, v1.4s");
    const uint64_t want = 0x5a10d206f365630dull;
    printf("native_simd_alias: %016llx %s\n", (unsigned long long)h, h == want ? "ALL PASS" : "FAILED");
    return h == want ? 0 : 1;
}
