// jit_ccmp.c — Test 32-bit CCMP/CCMN flag computation.
//
// This test validates the Turn 56 fix: the JIT was using 64-bit sub/add
// for 32-bit CCMP/CCMN, which computed the Sign Flag from bit 63 instead
// of bit 31. For a 32-bit operation like `ccmp w3, #2, #0, cs` where
// w3=0xFFFFFFFF, the 64-bit sub gives SF=0 (positive) while the 32-bit
// sub gives SF=1 (negative). This caused the ARM N flag to be wrong.
//
// Build: make cross SRC=ctest/jit_ccmp.c OUT=ctest/jit_ccmp.elf
// Run:   ./bifrost-emu ctest/jit_ccmp.elf
//        ./bifrost-emu --no-jit ctest/jit_ccmp.elf
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>

static int passes = 0;
static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passes++; write(2, "PASS: " msg "\n", 7 + sizeof(msg)); } \
    else { failures++; write(2, "FAIL: " msg "\n", 7 + sizeof(msg)); } \
} while (0)

// Test CCMP with 32-bit values that have bit 31 set.
// The JIT must compute N from bit 31, not bit 63.
static int test_ccmp_32bit_negative(void) {
    // Set up: w0 = 0xFFFFFFFF (32-bit, bit 31 set, but zero-extended to 64 bits
    // so bit 63 is clear). cmp w0, #0 → C=1 (no borrow, 0xFFFFFFFF >= 0).
    // Then ccmp w1, #2, #0, cs → since C=1, do cmp w1, #2.
    // If w1 = 0xFFFFFFFF: w1 - 2 = 0xFFFFFFFD. N=1 (bit 31 set), C=1 (no borrow).
    //
    // The bug: 64-bit sub gives 0x00000000FFFFFFFD, SF=0 → N=0 (wrong).
    // The fix: 32-bit sub gives 0xFFFFFFFD, SF=1 → N=1 (correct).
    uint32_t w0 = 0xFFFFFFFF;
    uint32_t w1 = 0xFFFFFFFF;
    uint64_t pstate;

    __asm__ volatile (
        "mov w0, %w[val0]\n"
        "mov w1, %w[val1]\n"
        "cmp w0, #0\n"           // sets C=1 (w0 >= 0)
        "ccmp w1, #2, #0, cs\n"  // CS=true → cmp w1, #2
        "mrs %[ps], nzcv\n"      // read NZCV flags
        : [ps] "=r"(pstate)
        : [val0] "r"(w0), [val1] "r"(w1)
        : "w0", "w1", "cc", "memory"
    );

    uint64_t n = (pstate >> 31) & 1;
    uint64_t c = (pstate >> 29) & 1;
    char msg[128];
    int len = snprintf(msg, sizeof(msg), "ccmp 32-bit neg: N=%lld C=%lld pstate=0x%llx\n",
                       (unsigned long long)n, (unsigned long long)c,
                       (unsigned long long)pstate);
    write(2, msg, len);
    return (n == 1 && c == 1);
}

// Test CCMP with 32-bit values that are small positive.
static int test_ccmp_32bit_positive(void) {
    uint32_t w0 = 0xFFFFFFFF;
    uint32_t w1 = 5;  // w1 - 2 = 3, positive
    uint64_t pstate;

    __asm__ volatile (
        "mov w0, %w[val0]\n"
        "mov w1, %w[val1]\n"
        "cmp w0, #0\n"
        "ccmp w1, #2, #0, cs\n"
        "mrs %[ps], nzcv\n"
        : [ps] "=r"(pstate)
        : [val0] "r"(w0), [val1] "r"(w1)
        : "w0", "w1", "cc", "memory"
    );

    uint64_t n = (pstate >> 31) & 1;
    uint64_t c = (pstate >> 29) & 1;
    char msg[128];
    int len = snprintf(msg, sizeof(msg), "ccmp 32-bit pos: N=%lld C=%lld pstate=0x%llx\n",
                       (unsigned long long)n, (unsigned long long)c,
                       (unsigned long long)pstate);
    write(2, msg, len);
    return (n == 0 && c == 1);
}

// Test CCMP condition FALSE path (NZCV = immediate).
// CCMP semantics: if condition is TRUE → do the compare; if FALSE → set NZCV=imm.
// To test the FALSE path, we need the condition to be FALSE.
// NE is FALSE when Z=1 (equal). So: cmp w0, #5 with w0=5 → Z=1 → NE=false.
// Then ccmp w1, #2, #0xf, ne → NE=false → set NZCV=0xF.
static int test_ccmp_cond_false(void) {
    volatile uint32_t w0_val = 5;  // equal to cmp value → Z=1 → NE=false
    volatile uint32_t w1 = 42;
    volatile uint64_t pstate = 0;

    __asm__ volatile (
        "mov w0, %w[v0]\n"
        "mov w1, %w[v1]\n"
        "cmp w0, #5\n"            // 5-5=0: Z=1, NE is FALSE
        "ccmp w1, #2, #0xf, ne\n" // NE=false → set NZCV=0xF
        "mrs %[ps], nzcv\n"
        : [ps] "=r"(pstate)
        : [v0] "r"((uint32_t)w0_val), [v1] "r"((uint32_t)w1)
        : "w0", "w1", "cc", "memory"
    );

    // NZCV = 0xF = N=1, Z=1, C=1, V=1
    uint64_t n = (pstate >> 31) & 1;
    uint64_t z = (pstate >> 30) & 1;
    uint64_t c = (pstate >> 29) & 1;
    uint64_t v = (pstate >> 28) & 1;
    char msg[128];
    int len = snprintf(msg, sizeof(msg), "ccmp cond-false(ne): N=%lld Z=%lld C=%lld V=%lld pstate=0x%llx\n",
                       (unsigned long long)n, (unsigned long long)z,
                       (unsigned long long)c, (unsigned long long)v,
                       (unsigned long long)pstate);
    write(2, msg, len);
    return (n == 1 && z == 1 && c == 1 && v == 1);
}

// Test 64-bit CCMP still works.
static int test_ccmp_64bit(void) {
    uint64_t x0 = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t x1 = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t pstate;

    __asm__ volatile (
        "mov x0, %[val0]\n"
        "mov x1, %[val1]\n"
        "cmp x0, #0\n"
        "ccmp x1, #2, #0, cs\n"
        "mrs %[ps], nzcv\n"
        : [ps] "=r"(pstate)
        : [val0] "r"(x0), [val1] "r"(x1)
        : "x0", "x1", "cc", "memory"
    );

    uint64_t n = (pstate >> 31) & 1;
    uint64_t c = (pstate >> 29) & 1;
    char msg[128];
    int len = snprintf(msg, sizeof(msg), "ccmp 64-bit: N=%lld C=%lld pstate=0x%llx\n",
                       (unsigned long long)n, (unsigned long long)c,
                       (unsigned long long)pstate);
    write(2, msg, len);
    // x1=0xFFFFFFFFFFFFFFFF, x1-2 = 0xFFFFFFFFFFFFFFFD. N=1, C=1 (no borrow).
    return (n == 1 && c == 1);
}

int main(void) {
    CHECK(test_ccmp_32bit_negative(), "32-bit CCMP with negative result: N=1 C=1");
    CHECK(test_ccmp_32bit_positive(), "32-bit CCMP with positive result: N=0 C=1");
    CHECK(test_ccmp_cond_false(), "CCMP condition false: NZCV=0xF");
    CHECK(test_ccmp_64bit(), "64-bit CCMP with negative result: N=1 C=1");

    char msg[128];
    int len = snprintf(msg, sizeof(msg), "passes=%d failures=%d\n", passes, failures);
    write(2, msg, len);
    if (failures == 0) {
        write(2, "ALL PASS\n", 9);
        return 0;
    }
    write(2, "SOME FAILURES\n", 14);
    return 1;
}
