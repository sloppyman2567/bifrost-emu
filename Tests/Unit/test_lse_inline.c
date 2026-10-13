// test_lse_inline.c — LSE atomic instruction test via inline assembly.
//
// Forces the compiler to emit LSE atomic instructions (CAS, LDADD, LDSET,
// LDCLR, LDEOR, SWP) using inline asm with -march=armv8.1-a+lse. This
// exercises the JIT's native LSE codegen paths directly, catching bugs
// that __sync_* builtins (which compile to LDXR/STXR loops) miss.
//
// Build: make cross SRC=Tests/Unit/test_lse_inline.c OUT=Tests/Unit/test_lse_inline.elf
//        (Makefile's cross target adds -march=armv8.1-a+lse via CFLAGS)
// Run:   ./bifrost-emu Tests/Unit/test_lse_inline.elf
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// IL2CPP's Boehm GC lock: SWPAB acquire, 64-bit STLR release. A release
// written twice by the host reopened a lock another thread had just taken.
#define LOCK_THREADS 4
#define LOCK_ITERS 2000000
static volatile uint64_t lock_word;
static volatile uint64_t locked_count;

static void* lock_worker(void* arg) {
    (void)arg;
    for (int i = 0; i < LOCK_ITERS; i++) {
        uint32_t old;
        do {
            __asm__ volatile("swpab %w1, %w0, [%2]"
                             : "=&r"(old) : "r"(1u), "r"(&lock_word) : "memory");
        } while (old);
        locked_count = locked_count + 1;
        __asm__ volatile("stlr xzr, [%0]" :: "r"(&lock_word) : "memory");
    }
    return NULL;
}

int main(void) {
    int failures = 0;

    {
        pthread_t t[LOCK_THREADS];
        for (int i = 0; i < LOCK_THREADS; i++) pthread_create(&t[i], NULL, lock_worker, NULL);
        for (int i = 0; i < LOCK_THREADS; i++) pthread_join(t[i], NULL);
        uint64_t expected = (uint64_t)LOCK_THREADS * LOCK_ITERS;
        if (locked_count != expected) {
            printf("SWPAB lock/STLR unlock: %llu (exp %llu) FAIL\n",
                   (unsigned long long)locked_count, (unsigned long long)expected);
            failures++;
        } else {
            printf("SWPAB lock/STLR unlock: %llu OK\n", (unsigned long long)expected);
        }
    }

    // Chromium's renderer uses LDSETH for 16-bit flags. Check the complete
    // X result, a dirty upper operand, and both neighboring halfwords.
#define CHECK_HALFWORD(op, initial, operand, expected) do { \
        uint16_t words[] = {0x1234, (initial), 0xabcd}; \
        uint64_t old = UINT64_MAX, source = UINT64_C(0xdeadbeef98760000) | (operand); \
        __asm__ volatile(".arch_extension lse\n\t" #op " %w1, %w0, [%2]" \
                : "=&r"(old) : "r"(source), "r"(&words[1]) : "memory"); \
        if (old != (initial) || words[1] != (expected) || \
                words[0] != 0x1234 || words[2] != 0xabcd) { \
            printf(#op ": old=0x%llx memory=%04x:%04x:%04x FAIL\n", \
                    (unsigned long long)old, words[0], words[1], words[2]); \
            failures++; \
        } \
    } while (0)
    CHECK_HALFWORD(ldseth, 4, 2, 6);
    CHECK_HALFWORD(ldclrh, 0xff81, 0x0081, 0xff00);
    CHECK_HALFWORD(ldeorh, 0x81f0, 0x00ff, 0x810f);
    CHECK_HALFWORD(ldaddh, 0xfffe, 3, 1);
    CHECK_HALFWORD(swph, 0x8001, 0x7f23, 0x7f23);
#undef CHECK_HALFWORD

    // ── CAS (compare-and-swap) ──
    {
        uint64_t val = 42;
        uint64_t expected = 42, desired = 99;
        uint64_t old;
        __asm__ volatile ("cas %0, %1, [%2]"
                          : "+r"(expected) : "r"(desired), "r"(&val) : "memory");
        old = expected;
        if (val != 99 || old != 42) {
            printf("CAS: val=%llu old=%llu (exp val=99 old=42) FAIL\n",
                   (unsigned long long)val, (unsigned long long)old);
            failures++;
        } else {
            printf("CAS: val=99 old=42 OK\n");
        }
    }

    // ── LDADD (load-add, returns old value) ──
    {
        uint64_t val = 10;
        uint64_t old = 0;
        __asm__ volatile ("ldadd %1, %0, [%2]"
                          : "=r"(old) : "r"((uint64_t)5), "r"(&val) : "memory");
        if (val != 15 || old != 10) {
            printf("LDADD: val=%llu old=%llu (exp val=15 old=10) FAIL\n",
                   (unsigned long long)val, (unsigned long long)old);
            failures++;
        } else {
            printf("LDADD: val=15 old=10 OK\n");
        }
    }

    // ── STADD (store-add, no return value) ──
    {
        uint64_t val = 100;
        __asm__ volatile ("stadd %0, [%1]"
                          :: "r"((uint64_t)7), "r"(&val) : "memory");
        if (val != 107) {
            printf("STADD: val=%llu (exp 107) FAIL\n", (unsigned long long)val);
            failures++;
        } else {
            printf("STADD: val=107 OK\n");
        }
    }

    // ── LDSET (load-set, returns old, ORs in new) ──
    {
        uint64_t val = 0xF0;
        uint64_t old = 0;
        __asm__ volatile ("ldset %1, %0, [%2]"
                          : "=r"(old) : "r"((uint64_t)0x0F), "r"(&val) : "memory");
        if (val != 0xFF || old != 0xF0) {
            printf("LDSET: val=0x%llx old=0x%llx (exp val=0xff old=0xf0) FAIL\n",
                   (unsigned long long)val, (unsigned long long)old);
            failures++;
        } else {
            printf("LDSET: val=0xff old=0xf0 OK\n");
        }
    }

    // ── LDCLR (load-clear, returns old, ANDs in ~new) ──
    {
        uint64_t val = 0xFF;
        uint64_t old = 0;
        __asm__ volatile ("ldclr %1, %0, [%2]"
                          : "=r"(old) : "r"((uint64_t)0x0F), "r"(&val) : "memory");
        if (val != 0xF0 || old != 0xFF) {
            printf("LDCLR: val=0x%llx old=0x%llx (exp val=0xf0 old=0xff) FAIL\n",
                   (unsigned long long)val, (unsigned long long)old);
            failures++;
        } else {
            printf("LDCLR: val=0xf0 old=0xff OK\n");
        }
    }

    // ── LDEOR (load-eor, returns old, XORs in new) ──
    {
        uint64_t val = 0xAA;
        uint64_t old = 0;
        __asm__ volatile ("ldeor %1, %0, [%2]"
                          : "=r"(old) : "r"((uint64_t)0xFF), "r"(&val) : "memory");
        if (val != 0x55 || old != 0xAA) {
            printf("LDEOR: val=0x%llx old=0x%llx (exp val=0x55 old=0xaa) FAIL\n",
                   (unsigned long long)val, (unsigned long long)old);
            failures++;
        } else {
            printf("LDEOR: val=0x55 old=0xaa OK\n");
        }
    }

    // ── SWP (swap, returns old) ──
    {
        uint64_t val = 77;
        uint64_t old = 0;
        __asm__ volatile ("swp %1, %0, [%2]"
                          : "=r"(old) : "r"((uint64_t)88), "r"(&val) : "memory");
        if (val != 88 || old != 77) {
            printf("SWP: val=%llu old=%llu (exp val=88 old=77) FAIL\n",
                   (unsigned long long)val, (unsigned long long)old);
            failures++;
        } else {
            printf("SWP: val=88 old=77 OK\n");
        }
    }

    // ── 32-bit variants ──
    {
        uint32_t val = 10;
        uint32_t old = 0;
        __asm__ volatile ("ldadd %w1, %w0, [%2]"
                          : "=r"(old) : "r"((uint32_t)5), "r"(&val) : "memory");
        if (val != 15 || old != 10) {
            printf("LDADD(32): val=%u old=%u (exp val=15 old=10) FAIL\n", val, old);
            failures++;
        } else {
            printf("LDADD(32): val=15 old=10 OK\n");
        }
    }

    if (failures == 0) {
        printf("test_lse_inline: ALL PASS\n");
        return 0;
    } else {
        printf("test_lse_inline: %d FAILURES\n", failures);
        return 1;
    }
}
