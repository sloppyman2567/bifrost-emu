// jit_vector_cold_step.c — a scalar FP cold step must not make clobbered
// vector-cache slots look resident on the hot path that skips it.
//
// A native USHL clobbers XMMs that cache v1/v2; they become
// memory-authoritative. FADD's NaN cold step reloads the cache after its
// interpreter step. That reload is skipped at runtime when FADD is not NaN,
// so it must not mark the clobbered slots valid: the following ADDs must
// reload v1/v2 from memory. Expected values match qemu-aarch64.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const uint32_t in[4] = {1, 2, 3, 4}, sh[4] = {1, 1, 1, 1};
    uint32_t out[3][4];
    float f[3] = {1.5f, 2.25f, 0};
    __asm__ volatile(
        "ldr q1, [%[in]]\n"
        "ldr q8, [%[in]]\n"
        "ldr q9, [%[sh]]\n"
        "movi v2.4s, #0\n"
        "movi v3.4s, #0\n"
        "ldr s21, [%[f]]\n"
        "ldr s22, [%[f], #4]\n"
        "mov x10, #3\n"
        "1:\n"
        "add v2.4s, v2.4s, v1.4s\n"
        "add v2.4s, v2.4s, v1.4s\n"
        "add v2.4s, v2.4s, v1.4s\n"
        "ushl v7.4s, v8.4s, v9.4s\n"
        "fadd s20, s21, s22\n"
        "add v3.4s, v3.4s, v1.4s\n"
        "add v3.4s, v3.4s, v2.4s\n"
        "subs x10, x10, #1\n"
        "b.ne 1b\n"
        "str q2, [%[out]]\n"
        "str q3, [%[out], #16]\n"
        "str q7, [%[out], #32]\n"
        "str s20, [%[f], #8]\n"
        :
        : [in] "r"(in), [sh] "r"(sh), [out] "r"(out), [f] "r"(f)
        : "x10", "v1", "v2", "v3", "v7", "v8", "v9", "v20", "v21", "v22", "memory", "cc");
    static const uint32_t want[3][4] = {{9, 18, 27, 36}, {21, 42, 63, 84}, {2, 4, 6, 8}};
    int ok = memcmp(out, want, sizeof(want)) == 0 && f[2] == 3.75f;
    if (!ok)
        for (int r = 0; r < 3; ++r)
            printf("row %d: %u %u %u %u\n", r, out[r][0], out[r][1], out[r][2], out[r][3]);
    printf("vector_cold_step: %s\n", ok ? "ALL PASS" : "FAILED");
    return ok ? 0 : 1;
}
