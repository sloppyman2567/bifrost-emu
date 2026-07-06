// jit_scvtf_fp.c — Test SCVTF/UCVTF (scalar, FP source) and
// FCVTZS/FCVTZU (scalar, FP dest).
//
// These are in the "Advanced SIMD scalar two-register miscellaneous"
// group (high byte 0x5E). They differ from the standard FP scalar
// forms (0x1E...) by having the integer source/dest in an FP register
// instead of a GPR. GCC/clang emit these for `(double)long_var` when
// the long is already in an FP register from a load.
//
// Before this fix, the emulator silently NOP'd these instructions,
// causing `(double)19` to return 0.0 (the bit pattern of 19 reinterpreted
// as IEEE 754 double is a tiny denormal that prints as 0.000000).
// This broke toybox `time` (showed user=0.000, sys=0.000 even for
// CPU-intensive commands) because the rusage delta computation used
// SCVTF to convert tv_sec/tv_usec to doubles.
//
// Build: make cross SRC=ctest/jit_scvtf_fp.c OUT=ctest/jit_scvtf_fp.elf
// Run:   ./bifrost-emu ctest/jit_scvtf_fp.elf
//        ./bifrost-emu --no-jit ctest/jit_scvtf_fp.elf
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

int main(void) {
    char msg[128];
    int n;

    // 1. SCVTF D0, D0 — 64-bit signed int → double (the main bug)
    // The compiler emits this for `(double)long_var` when the long
    // is already in d0 from a load.
    long a = 19;
    double da;
    __asm__ volatile (
        "ldr d0, %[a]\n"
        "scvtf d0, d0\n"
        "str d0, %[da]\n"
        : [da] "=m"(da), [a] "+m"(a)
        :
        : "v0", "memory"
    );
    n = snprintf(msg, sizeof(msg), "scvtf d0,d0 (19) = %f\n", da);
    write(2, msg, n);
    CHECK(da == 19.0, "SCVTF D0,D0 (long→double) gives 19.0");

    // 2. SCVTF with a larger value
    long b = 1234567890;
    double db;
    __asm__ volatile (
        "ldr d0, %[b]\n"
        "scvtf d0, d0\n"
        "str d0, %[db]\n"
        : [db] "=m"(db), [b] "+m"(b)
        :
        : "v0", "memory"
    );
    n = snprintf(msg, sizeof(msg), "scvtf d0,d0 (1234567890) = %f\n", db);
    write(2, msg, n);
    CHECK(db == 1234567890.0, "SCVTF D0,D0 (large long→double)");

    // 3. SCVTF with negative value
    long c = -42;
    double dc;
    __asm__ volatile (
        "ldr d0, %[c]\n"
        "scvtf d0, d0\n"
        "str d0, %[dc]\n"
        : [dc] "=m"(dc), [c] "+m"(c)
        :
        : "v0", "memory"
    );
    n = snprintf(msg, sizeof(msg), "scvtf d0,d0 (-42) = %f\n", dc);
    write(2, msg, n);
    CHECK(dc == -42.0, "SCVTF D0,D0 (negative long→double)");

    // 4. UCVTF D0, D0 — 64-bit unsigned int → double
    uint64_t u = 12345;
    double du;
    __asm__ volatile (
        "ldr d0, %[u]\n"
        "ucvtf d0, d0\n"
        "str d0, %[du]\n"
        : [du] "=m"(du), [u] "+m"(u)
        :
        : "v0", "memory"
    );
    n = snprintf(msg, sizeof(msg), "ucvtf d0,d0 (12345) = %f\n", du);
    write(2, msg, n);
    CHECK(du == 12345.0, "UCVTF D0,D0 (uint64→double)");

    // 5. UCVTF with a value > INT64_MAX (would be negative if signed)
    uint64_t big = 0xFFFFFFFFFFFFFFFFULL;
    double dbig;
    __asm__ volatile (
        "ldr d0, %[big]\n"
        "ucvtf d0, d0\n"
        "str d0, %[dbig]\n"
        : [dbig] "=m"(dbig), [big] "+m"(big)
        :
        : "v0", "memory"
    );
    n = snprintf(msg, sizeof(msg), "ucvtf d0,d0 (0xFFFFFFFFFFFFFFFF) = %f\n", dbig);
    write(2, msg, n);
    CHECK(dbig > 1.8e19, "UCVTF D0,D0 (large uint64→double)");

    // 6. SCVTF S0, S0 — 32-bit signed int → single-precision float
    int i32 = 42;
    float fi;
    __asm__ volatile (
        "ldr s0, %[i32]\n"
        "scvtf s0, s0\n"
        "str s0, %[fi]\n"
        : [fi] "=m"(fi), [i32] "+m"(i32)
        :
        : "v0", "memory"
    );
    n = snprintf(msg, sizeof(msg), "scvtf s0,s0 (42) = %f\n", (double)fi);
    write(2, msg, n);
    CHECK(fi == 42.0f, "SCVTF S0,S0 (int→float)");

    // 7. FCVTZS D0, D0 — double → 64-bit signed int (FP dest)
    double x = 42.7;
    int64_t xi;
    __asm__ volatile (
        "ldr d0, %[x]\n"
        "fcvtzs d0, d0\n"
        "str d0, %[xi]\n"
        : [xi] "=m"(xi), [x] "+m"(x)
        :
        : "v0", "memory"
    );
    n = snprintf(msg, sizeof(msg), "fcvtzs d0,d0 (42.7) = %ld\n", (long)xi);
    write(2, msg, n);
    CHECK(xi == 42, "FCVTZS D0,D0 (double→long, truncates)");

    // 8. FCVTZS with negative double
    double y = -3.9;
    int64_t yi;
    __asm__ volatile (
        "ldr d0, %[y]\n"
        "fcvtzs d0, d0\n"
        "str d0, %[yi]\n"
        : [yi] "=m"(yi), [y] "+m"(y)
        :
        : "v0", "memory"
    );
    n = snprintf(msg, sizeof(msg), "fcvtzs d0,d0 (-3.9) = %ld\n", (long)yi);
    write(2, msg, n);
    CHECK(yi == -3, "FCVTZS D0,D0 (negative double→long, truncates toward 0)");

    // 9. FCVTZU D0, D0 — double → 64-bit unsigned int (FP dest)
    double p = 99.5;
    uint64_t pu;
    __asm__ volatile (
        "ldr d0, %[p]\n"
        "fcvtzu d0, d0\n"
        "str d0, %[pu]\n"
        : [pu] "=m"(pu), [p] "+m"(p)
        :
        : "v0", "memory"
    );
    n = snprintf(msg, sizeof(msg), "fcvtzu d0,d0 (99.5) = %lu\n", (unsigned long)pu);
    write(2, msg, n);
    CHECK(pu == 99, "FCVTZU D0,D0 (double→uint64, truncates)");

    // 10. Compiler-emitted (double)long_var pattern — end-to-end test
    long val = 12345;
    double dval = (double)val;
    n = snprintf(msg, sizeof(msg), "(double)12345 = %f\n", dval);
    write(2, msg, n);
    CHECK(dval == 12345.0, "Compiler-emitted (double)long pattern works");

    // 11. The original bug: rusage-style delta computation
    long sec_a = 5, usec_a = 100000;
    long sec_b = 8, usec_b = 200000;
    double ta = (double)sec_a + (double)usec_a / 1e6;
    double tb = (double)sec_b + (double)usec_b / 1e6;
    double delta = tb - ta;
    n = snprintf(msg, sizeof(msg), "rusage delta = %f\n", delta);
    write(2, msg, n);
    CHECK(delta > 3.0 && delta < 3.2, "Rusage delta computation works");

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
