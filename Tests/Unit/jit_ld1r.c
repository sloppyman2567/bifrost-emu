// jit_ld1r.c — LD1R {Vt.T}, [Xn] (load one element, replicate to every lane).
// The JIT emits it as a guarded element load plus DUP; .1D keeps the helper.
// Covers every arrangement, no writeback, post-index by the element size and by
// a register, SP as the base, Vt in v16-v31, a loop whose vectors stay cached,
// and a fault reported at the LD1R with Xn not yet moved.
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

static int checks, fails;
#define CHECK(ok, ...) do { ++checks; if (!(ok)) { ++fails; printf("NG " __VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t src[16] = {0x81, 0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8,
                                0x19, 0x2a, 0x3b, 0x4c, 0x5d, 0x6e, 0x7f, 0x80};
typedef struct { uint64_t lo, hi; } V128;

static V128 expect(int esize, int q, const uint8_t *p) {
    uint8_t b[16] = {0};
    for (int i = 0; i < (q ? 16 : 8); i++) b[i] = p[i % esize];
    V128 v; memcpy(&v, b, 16); return v;
}
// Vt = v17 (dirty beforehand); returns Vt and the final base.
#define FORM(name, T, POST, ...) static V128 name(const uint8_t *p, uint64_t *base_out) { \
    V128 r; const uint8_t *b = p; \
    __asm__ volatile("movi v17.16b, #0x55\n ld1r {v17." T "}, [%[b]]" POST "\n str q17, [%[r]]" \
        : [b] "+r"(b) : [r] "r"(&r), ##__VA_ARGS__ : "memory", "v17"); \
    *base_out = (uint64_t)b; return r; }
FORM(f8b, "8b", "") FORM(f16b, "16b", ", #1") FORM(f4h, "4h", ", #2") FORM(f8h, "8h", "")
FORM(f2s, "2s", ", #4") FORM(f4s, "4s", "") FORM(f1d, "1d", ", #8") FORM(f2d, "2d", ", #8")
FORM(f4s_reg, "4s", ", %[step]", [step] "r"((uint64_t)40))
static const struct { V128 (*fn)(const uint8_t *, uint64_t *); int esize, q; uint64_t advance; const char *name; } forms[] = {
    {f8b, 1, 0, 0, "8b"}, {f16b, 1, 1, 1, "16b #1"}, {f4h, 2, 0, 2, "4h #2"}, {f8h, 2, 1, 0, "8h"},
    {f2s, 4, 0, 4, "2s #4"}, {f4s, 4, 1, 0, "4s"}, {f1d, 8, 0, 8, "1d #8"}, {f2d, 8, 1, 8, "2d #8"},
    {f4s_reg, 4, 1, 40, "4s x"},
};

// SP as the base: the element sits at [sp].
static V128 from_sp(uint32_t word) {
    V128 r;
    __asm__ volatile("sub sp, sp, #16\n str %w[w], [sp]\n ld1r {v20.4s}, [sp]\n add sp, sp, #16\n str q20, [%[r]]"
        : : [w] "r"(word), [r] "r"(&r) : "memory", "v20");
    return r;
}

// Sum of LD1R'd elements times a vector, in a loop of vector ops only.
static float loop_sum(const float *a, int n) {
    float out;
    const float *p = a;
    __asm__ volatile("movi v0.4s, #0\n fmov v1.4s, #1.0\n mov x9, %[n]\n"
        "1: ld1r {v2.4s}, [%[p]], #4\n fmla v0.4s, v2.4s, v1.4s\n fadd v1.4s, v1.4s, v1.4s\n subs x9, x9, #1\n b.ne 1b\n"
        "faddp v0.4s, v0.4s, v0.4s\n faddp s0, v0.2s\n fmov %w[o], s0"
        : [o] "=r"(out), [p] "+r"(p) : [n] "r"((long)n) : "memory", "cc", "x9", "v0", "v1", "v2");
    return out;
}

static sigjmp_buf jump;
static volatile uint64_t fault_x20, fault_pc;
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    fault_x20 = uc->uc_mcontext.regs[20];
    fault_pc = uc->uc_mcontext.pc;
    siglongjmp(jump, 1);
}
static int fault_case(uint8_t *bad) {
    static uint64_t before_pc;
    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    if (sigsetjmp(jump, 1) == 0) {
        __asm__ volatile("mov x20, %[b]\n adr x21, 2f\n str x21, [%[pc]]\n 2: ld1r {v7.4s}, [x20], #4"
            : : [b] "r"(bad), [pc] "r"(&before_pc) : "memory", "x20", "x21", "v7");
        return 0;  // no fault: wrong
    }
    return fault_x20 == (uint64_t)bad && fault_pc == before_pc;
}

int main(void) {
    for (int off = 0; off < 8; off++)
        for (size_t f = 0; f < sizeof forms / sizeof forms[0]; f++) {
            uint64_t base;
            const V128 got = forms[f].fn(src + off, &base);
            const V128 want = expect(forms[f].esize, forms[f].q, src + off);
            CHECK(!memcmp(&got, &want, 16), "%s off %d: %016llx:%016llx want %016llx:%016llx", forms[f].name, off,
                  (unsigned long long)got.hi, (unsigned long long)got.lo, (unsigned long long)want.hi,
                  (unsigned long long)want.lo);
            CHECK(base == (uint64_t)(src + off) + forms[f].advance, "%s off %d: base moved by %lld", forms[f].name,
                  off, (long long)(base - (uint64_t)(src + off)));
        }
    const V128 s = from_sp(0x3f8000c1u);
    CHECK(s.lo == 0x3f8000c13f8000c1ull && s.hi == s.lo, "sp base %016llx:%016llx", (unsigned long long)s.hi,
          (unsigned long long)s.lo);
    float a[64], want = 0, scale = 1;
    for (int i = 0; i < 64; i++) a[i] = (float)(i % 7) - 2.5f;
    for (int i = 0; i < 64; i++) { want += a[i] * scale; scale += scale; }
    const float got = loop_sum(a, 64);
    CHECK(got == want * 4, "loop %.9g want %.9g", got, want * 4);
    uint8_t *page = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED && fault_case(page + 8), "fault: x20 %llx pc %llx",
          (unsigned long long)fault_x20, (unsigned long long)fault_pc);
    if (fails) { printf("jit_ld1r: FAILED (%d of %d)\n", fails, checks); return 1; }
    printf("jit_ld1r: ALL PASS (%d checks)\n", checks);
    return 0;
}
