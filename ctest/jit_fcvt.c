// jit_fcvt.c — Test FCVT (float↔double) and FP load/store.
//
// This test validates the Turn 57 fix: FCVT was misidentified as SCVTF
// because the SCVTF mask (0x7F3E0000) also matches FCVT (0x1E22C000).
// The FCVT check must come BEFORE the SCVTF check in the IR translator.
//
// Also tests that FP loads/stores via LDR/STR correctly access v_lo[]
// instead of cpu.regs[] (the Turn 57 FP register load/store fix).
//
// Build: make cross SRC=ctest/jit_fcvt.c OUT=ctest/jit_fcvt.elf
// Run:   ./bifrost-emu ctest/jit_fcvt.elf
//        ./bifrost-emu --no-jit ctest/jit_fcvt.elf
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

static int passes = 0;
static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passes++; write(2, "PASS: " msg "\n", 7 + sizeof(msg)); } \
    else { failures++; write(2, "FAIL: " msg "\n", 7 + sizeof(msg)); } \
} while (0)

// Global float/double for testing FP loads from memory.
static float g_float = 42.0f;
static double g_double = 3.14;

int main(void) {
    char msg[128];
    int n;

    // 1. FCVT D0, S0 (single → double) — the main bug.
    // Before fix: FCVT was misidentified as SCVTF, reading x0 instead of v0.
    float f = 42.0f;
    double d;
    __asm__ volatile (
        "ldr s0, %[f]\n"
        "fcvt d0, s0\n"
        "str d0, %[d]\n"
        : [d] "=m"(d), [f] "+m"(f)
        :
        : "v0", "memory"
    );
    n = snprintf(msg, sizeof(msg), "fcvt d0,s0 (42.0f) = %f\n", d);
    write(2, msg, n);
    CHECK(d == 42.0, "FCVT D0,S0 (single→double) gives 42.0");

    // 2. FCVT S0, D0 (double → single)
    double d2 = 1.5;
    float f2;
    __asm__ volatile (
        "ldr d0, %[d]\n"
        "fcvt s0, d0\n"
        "str s0, %[f]\n"
        : [f] "=m"(f2), [d] "+m"(d2)
        :
        : "v0", "memory"
    );
    n = snprintf(msg, sizeof(msg), "fcvt s0,d0 (1.5) = %f\n", (double)f2);
    write(2, msg, n);
    CHECK(f2 == 1.5f, "FCVT S0,D0 (double→single) gives 1.5");

    // 3. Load float from global, convert to double
    double d3 = (double)g_float;
    n = snprintf(msg, sizeof(msg), "(double)g_float(42.0f) = %f\n", d3);
    write(2, msg, n);
    CHECK(d3 == 42.0, "global float→double conversion");

    // 4. Load double from global
    double d4 = g_double;
    n = snprintf(msg, sizeof(msg), "g_double = %f\n", d4);
    write(2, msg, n);
    CHECK(d4 == 3.14, "global double load");

    // 5. SCVTF still works (not broken by FCVT fix)
    int i = 42;
    float f5;
    __asm__ volatile (
        "ldr w0, %[i]\n"
        "scvtf s0, w0\n"
        "str s0, %[f]\n"
        : [f] "=m"(f5), [i] "+m"(i)
        :
        : "w0", "v0", "memory"
    );
    n = snprintf(msg, sizeof(msg), "scvtf s0,w0 (42) = %f\n", (double)f5);
    write(2, msg, n);
    CHECK(f5 == 42.0f, "SCVTF S0,W0 (int→float) still works");

    // 6. FP arithmetic: 3.14 + 1.0 = 4.14
    float a = 3.14f, b = 1.0f, c;
    __asm__ volatile (
        "ldr s0, %[a]\n"
        "ldr s1, %[b]\n"
        "fadd s0, s0, s1\n"
        "str s0, %[c]\n"
        : [c] "=m"(c), [a] "+m"(a), [b] "+m"(b)
        :
        : "v0", "v1", "memory"
    );
    n = snprintf(msg, sizeof(msg), "3.14f + 1.0f = %f\n", (double)c);
    write(2, msg, n);
    CHECK(c > 4.13f && c < 4.15f, "FADD S0 (float addition)");

    // Final summary.
    n = snprintf(msg, sizeof(msg), "passes=%d failures=%d\n", passes, failures);
    write(2, msg, n);
    if (failures == 0) {
        write(2, "ALL PASS\n", 9);
        return 0;
    }
    write(2, "SOME FAILURES\n", 14);
    return 1;
}
