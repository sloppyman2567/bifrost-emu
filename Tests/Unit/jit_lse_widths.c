/*
 * jit_lse_widths.c — every LSE atomic at every access size.
 *
 * Byte atomics, sub-word CAS and SMAX/SMIN/UMAX/UMIN used to step the
 * interpreter, and a successful 32-bit CAS left Ws's upper half dirty
 * (x86 cmpxchg does not write RAX on success). Each LD/ST form, SWP and
 * CAS (plain and ordered) runs with dirty upper register bits against a C
 * reference; the neighbouring bytes of the target must stay intact. Two
 * threads then hammer adjacent bytes and a shared halfword maximum. The
 * expected results also pass under QEMU.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int checks, fails;

/* x0 = Rs, x1 = Rt, x2 = address (rs=0, rn=2, rt=1 or 31). */
#define RUN(word) __asm__ volatile("mov x0, %[a]\n mov x1, %[b]\n mov x2, %[p]\n" \
    ".inst " #word "\n mov %[a], x0\n mov %[b], x1" \
    : [a] "+r"(a), [b] "+r"(b) : [p] "r"(p) : "x0", "x1", "x2", "memory")
#define XRUN(word) RUN(word)
#define LD(s, opc) ((s) << 30 | 0x38200041 | (opc) << 12)
#define ST(s, opc) ((s) << 30 | 0x3820005f | (opc) << 12)
#define SWP(s) ((s) << 30 | 0x38208041)
#define CAS(s, ord) ((s) << 30 | 0x08a07c41 | (ord))
#define C(w) case (w): RUN(w); break;
#define OPS(s, macro) macro(s, 0) macro(s, 1) macro(s, 2) macro(s, 3) \
                      macro(s, 4) macro(s, 5) macro(s, 6) macro(s, 7)
#define LDC(s, opc) C(LD(s, opc))
#define STC(s, opc) C(ST(s, opc))
#define SIZE(s) OPS(s, LDC) OPS(s, STC) C(SWP(s)) \
                C(CAS(s, 0)) C(CAS(s, 0x00400000)) C(CAS(s, 0x00008000)) C(CAS(s, 0x00408000))

static void run(uint32_t word, uint64_t* pa, uint64_t* pb, void* p) {
    uint64_t a = *pa, b = *pb;
    switch (word) { SIZE(0) SIZE(1) SIZE(2) SIZE(3) }
    *pa = a; *pb = b;
}

static int64_t sext(uint64_t x, unsigned bits) {
    return bits == 64 ? (int64_t)x : (int64_t)(x << (64 - bits)) >> (64 - bits);
}

/* Reference: returns the new memory value and updates a (Rs) / b (Rt). */
static uint64_t ref(uint32_t word, uint64_t old, uint64_t* a, uint64_t* b) {
    const unsigned bits = 8u << (word >> 30);
    const uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
    if ((word & 0x3f000000) == 0x08000000) {   /* CAS */
        uint64_t mem = old == (*a & mask) ? (*b & mask) : old;
        *a = old;
        return mem;
    }
    const uint64_t s = *a & mask;
    uint64_t mem;
    if (word & 0x8000) mem = s;                /* SWP */
    else switch ((word >> 12) & 7) {
        case 0: mem = old + s; break;
        case 1: mem = old & ~s; break;
        case 2: mem = old ^ s; break;
        case 3: mem = old | s; break;
        case 4: mem = sext(old, bits) > sext(s, bits) ? old : s; break;
        case 5: mem = sext(old, bits) < sext(s, bits) ? old : s; break;
        case 6: mem = old > s ? old : s; break;
        default: mem = old < s ? old : s; break;
    }
    if ((word & 31) != 31) *b = old;
    return mem & mask;
}

static void check_word(uint32_t word, uint64_t memval, uint64_t a, uint64_t b) {
    const unsigned bytes = 1u << (word >> 30);
    uint8_t buf[32], want[32];
    for (int i = 0; i < 32; ++i) buf[i] = (uint8_t)(0x5a ^ i * 7);
    memcpy(buf + 8, &memval, bytes);
    memcpy(want, buf, 32);
    uint64_t old = 0, ra = a, rb = b;
    memcpy(&old, buf + 8, bytes);
    uint64_t mem = ref(word, old, &ra, &rb);
    memcpy(want + 8, &mem, bytes);
    run(word, &a, &b, buf + 8);
    ++checks;
    if (memcmp(buf, want, 32) || a != ra || b != rb) {
        uint64_t got = 0; memcpy(&got, buf + 8, bytes);
        printf("FAIL %08x mem=%llx want=%llx x0=%llx want=%llx x1=%llx want=%llx%s\n", word,
               (unsigned long long)got, (unsigned long long)mem, (unsigned long long)a,
               (unsigned long long)ra, (unsigned long long)b, (unsigned long long)rb,
               memcmp(buf, want, 8) || memcmp(buf + 8 + bytes, want + 8 + bytes, 24 - bytes)
                   ? " (neighbours changed)" : "");
        ++fails;
    }
}

static uint8_t bytes_shared[2];
static uint16_t half_max;
#define ITER 100000
static void* hammer(void* arg) {
    const uint64_t lane = (uint64_t)(uintptr_t)arg;
    for (uint64_t i = 0; i < ITER; ++i) {
        uint64_t a = 1, b = 0; void* p = &bytes_shared[lane];
        XRUN(LD(0, 0));                         /* LDADDB */
        a = (i * 2 + lane) >> 2; b = 0; p = &half_max;
        XRUN(ST(1, 6));                         /* STUMAXH */
    }
    return NULL;
}

int main(int argc, char** argv) {
    static const uint64_t vals[] = {
        0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000,
        0xffffffff, 0x7fffffffffffffffull, 0x8000000000000000ull, ~0ull,
        0x0123456789abcdefull, 0xfedcba9876543210ull,
    };
    const unsigned nv = sizeof(vals) / sizeof(vals[0]);
    for (unsigned s = 0; s < 4; ++s) {
        const uint32_t words[] = {
            LD(s, 0), LD(s, 1), LD(s, 2), LD(s, 3), LD(s, 4), LD(s, 5), LD(s, 6), LD(s, 7),
            ST(s, 0), ST(s, 1), ST(s, 2), ST(s, 3), ST(s, 4), ST(s, 5), ST(s, 6), ST(s, 7),
            SWP(s), CAS(s, 0), CAS(s, 0x00400000), CAS(s, 0x00008000), CAS(s, 0x00408000),
        };
        for (unsigned w = 0; w < sizeof(words) / sizeof(words[0]); ++w)
            for (unsigned i = 0; i < nv; ++i)
                for (unsigned j = 0; j < nv; ++j) {
                    /* Dirty upper bits; CAS also gets an equal-low-bits case. */
                    const uint64_t dirty = 0xdead5a5a00000000ull | 0xa5a50000ull;
                    const uint64_t mask = s == 3 ? ~0ull : (1ull << (8 << s)) - 1;
                    const uint64_t a = (vals[j] & mask) | (s == 3 ? 0 : dirty & ~mask);
                    const uint64_t b = vals[(i + j) % nv] ^ 0x3c3c3c3c3c3c3c3cull;
                    check_word(words[w], vals[i], a, b);
                    if (j == 0) check_word(words[w], vals[i], (vals[i] & mask) | (s == 3 ? 0 : dirty & ~mask), b);
                }
    }
    /* "single" skips the threads: differential replay needs one guest CPU. */
    const int threaded = !(argc > 1 && strcmp(argv[1], "single") == 0);
    pthread_t t[2];
    for (uintptr_t i = 0; threaded && i < 2; ++i) pthread_create(&t[i], NULL, hammer, (void*)i);
    for (int i = 0; threaded && i < 2; ++i) pthread_join(t[i], NULL);
    checks += threaded;
    if (threaded && (bytes_shared[0] != (uint8_t)ITER || bytes_shared[1] != (uint8_t)ITER ||
        half_max != (uint16_t)(((ITER - 1) * 2 + 1) >> 2))) {
        printf("FAIL contention bytes=%02x,%02x want %02x max=%04x want %04x\n",
               bytes_shared[0], bytes_shared[1], (uint8_t)ITER, half_max,
               (uint16_t)(((ITER - 1) * 2 + 1) >> 2));
        ++fails;
    }
    if (fails) { printf("jit_lse_widths: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("jit_lse_widths: %d checks passed\n", checks);
    return 0;
}
