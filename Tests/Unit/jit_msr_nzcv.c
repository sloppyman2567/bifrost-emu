// jit_msr_nzcv.c — `msr nzcv, xN` takes its value from xN even when the
// same JIT block overwrites xN right after. The JIT used to run MSR NZCV
// through the interpreter, which read cpu.regs[N]; optimize_ir had dropped
// that store because the following instruction rewrites xN, so NZCV got
// the register's previous value.
//
// Plain C + inline asm: also runs under qemu-aarch64 as an oracle.
#include <stdint.h>
#include <stdio.h>

static int failures, checks;

static void check(int ok, const char* what, uint64_t in, uint64_t got) {
    checks++;
    if (!ok && failures++ < 8)
        printf("FAIL %s: in=%llx got=%llx\n", what, (unsigned long long)in,
               (unsigned long long)got);
}

// msr, then overwrite the source register, then read NZCV back.
static uint64_t msr_clobber_mrs(uint64_t v) {
    uint64_t out, tmp;
    __asm__ volatile("mov %1, %2\n msr nzcv, %1\n add %1, %1, #8\n mrs %0, nzcv\n"
                     : "=r"(out), "=&r"(tmp) : "r"(v) : "cc");
    return out;
}

// Same, with flags pending from a cmp before the msr, consumed by csets.
static uint64_t msr_clobber_cset(uint64_t v) {
    uint64_t n, z, c, ov, tmp;
    __asm__ volatile("cmp %5, %5\n mov %4, %5\n msr nzcv, %4\n add %4, %4, #8\n"
                     "cset %0, mi\n cset %1, eq\n cset %2, cs\n cset %3, vs\n"
                     : "=r"(n), "=r"(z), "=r"(c), "=r"(ov), "=&r"(tmp) : "r"(v) : "cc");
    return n << 31 | z << 30 | c << 29 | ov << 28;
}

int main(void) {
    // Repeat so the blocks run as JIT code, not only on first entry.
    for (int iter = 0; iter < 2000; iter++) {
        for (uint64_t f = 0; f < 16; f++) {
            uint64_t v = f << 28 | 0x0FFFFFFFull;   // low bits must be ignored
            uint64_t want = f << 28;
            uint64_t got = msr_clobber_mrs(v);
            check(got == want, "msr/add/mrs", v, got);
            got = msr_clobber_cset(v);
            check(got == want, "cmp/msr/add/cset", v, got);
        }
    }
    if (failures) {
        printf("%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("%d/%d checks passed\n", checks, checks);
    return 0;
}
