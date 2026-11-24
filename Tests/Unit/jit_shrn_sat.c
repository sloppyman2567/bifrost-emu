/* Saturating narrowing shifts (SQSHRN, UQSHRN, SQRSHRN, UQRSHRN, SQSHRUN, SQRSHRUN), native in the JIT for 16- and 32-bit
 * source lanes. Every subop at every shift of every lane size, both destination halves and Vd == Vn, on lanes chosen at
 * the rounding, saturation and sign boundaries plus pseudo-random ones, against an exact C model: the result and
 * FPSR.QC. The instruction words are built at run time (the shift is an immediate) into a page that is then made
 * executable. A last loop keeps the source in a vector register across iterations. The model matches QEMU. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int checks, failures;

static const uint32_t BASE[6] = {0x0F009400, 0x2F009400, 0x0F009C00, 0x2F009C00, 0x2F008400, 0x2F008C00};
static const char* const NAME[6] = {"SQSHRN", "UQSHRN", "SQRSHRN", "UQRSHRN", "SQSHRUN", "SQRSHRUN"};

/* x0 = source, x1 = initial destination, x2 = result; returns FPSR. v0 = source, v1 = destination (or v0 for Vd == Vn). */
static uint32_t* emit_stub(uint32_t* p, uint32_t insn, int alias) {
    *p++ = 0x3DC00000;                    /* ldr  q0, [x0] */
    *p++ = 0x3DC00021;                    /* ldr  q1, [x1] */
    *p++ = 0xD51B443F;                    /* msr  fpsr, xzr */
    *p++ = insn;
    *p++ = 0xD53B4423;                    /* mrs  x3, fpsr */
    *p++ = alias ? 0x3D800040 : 0x3D800041; /* str q0 / q1, [x2] */
    *p++ = 0xAA0303E0;                    /* mov  x0, x3 */
    *p++ = 0xD65F03C0;                    /* ret */
    return p;
}

static void model(int subop, int esize, int shift, int q, const uint8_t src[16], const uint8_t dst0[16], uint8_t out[16], int* qc) {
    const int round = subop == 2 || subop == 3 || subop == 5, uns_src = subop == 1 || subop == 3;
    const int uns_dst = subop != 0 && subop != 2, W = esize * 8, D = W / 2;
    uint8_t res[8] = {0};
    *qc = 0;
    for (int i = 0; i < 16 / esize; i++) {
        uint64_t a = 0;
        memcpy(&a, src + i * esize, esize);
        __int128 v = uns_src ? (__int128)a : (__int128)((int64_t)(a << (64 - W)) >> (64 - W));
        if (round) v += (__int128)1 << (shift - 1);
        v >>= shift;
        const __int128 lo = uns_dst ? 0 : -((__int128)1 << (D - 1));
        const __int128 hi = uns_dst ? (((__int128)1 << D) - 1) : (((__int128)1 << (D - 1)) - 1);
        if (v < lo) { v = lo; *qc = 1; }
        if (v > hi) { v = hi; *qc = 1; }
        const uint64_t r = (uint64_t)v;
        memcpy(res + i * (esize / 2), &r, esize / 2);
    }
    if (q) { memcpy(out, dst0, 8); memcpy(out + 8, res, 8); }
    else { memcpy(out, res, 8); memset(out + 8, 0, 8); }
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

/* Lanes at the edges of the rounding and saturation decisions for this width and shift. */
static int lanes_for(int esize, int shift, uint64_t* list, int cap) {
    const int W = esize * 8, D = W / 2, mask_bits = W == 64 ? 64 : W;
    const uint64_t mask = mask_bits == 64 ? ~0ull : (1ull << mask_bits) - 1;
    int n = 0;
    uint64_t base[] = {0, 1, 2, 3, ~0ull, ~1ull, ~2ull, 1ull << (W - 1), (1ull << (W - 1)) - 1, (1ull << (W - 1)) + 1,
                       (1ull << D) - 1, (1ull << D), (1ull << (D - 1)) - 1, (1ull << (D - 1)), -(1ll << (D - 1)), -(1ll << (D - 1)) - 1};
    for (size_t i = 0; i < sizeof base / sizeof *base && n < cap; i++) list[n++] = base[i] & mask;
    /* k << shift, with the half step below, at and above it: the rounding boundary of each result k. */
    const int64_t ks[] = {0, 1, -1, 2, -2, 127, 128, 129, 255, 256, -127, -128, -129, -255, -256, 32767, 32768, -32768, -32769, 65535, 65536};
    for (size_t i = 0; i < sizeof ks / sizeof *ks && n + 4 < cap; i++) {
        const uint64_t k = (uint64_t)ks[i] << shift, half = shift > 0 ? 1ull << (shift - 1) : 0;
        list[n++] = (k + half - 1) & mask; list[n++] = (k + half) & mask; list[n++] = (k + half + 1) & mask; list[n++] = k & mask;
    }
    while (n < cap) list[n++] = next() & mask;
    return n;
}

typedef uint64_t (*stub_fn)(const void*, const void*, void*);
struct Case { int subop, esize, shift, q, alias; uint32_t* code; };

static void check(const struct Case* c, const uint8_t src[16], const uint8_t dst0[16], int batch) {
    uint8_t out[16], want[16];
    int qc;
    const uint8_t* seed = c->alias ? src : dst0;
    model(c->subop, c->esize, c->shift, c->q, src, seed, want, &qc);
    const uint64_t fpsr = ((stub_fn)c->code)(src, seed, out);
    checks++;
    if (memcmp(out, want, 16) || (int)((fpsr >> 27) & 1) != qc || (fpsr & ~(1ull << 27))) {
        failures++;
        if (failures < 20)
            printf("FAIL %s .%s shift=%d q=%d alias=%d batch=%d: qc %d (want %d) fpsr=%llx\n", NAME[c->subop],
                   c->esize == 2 ? "8h->8b" : c->esize == 4 ? "4s->4h" : "2d->2s", c->shift, c->q, c->alias, batch,
                   (int)((fpsr >> 27) & 1), qc, (unsigned long long)fpsr);
    }
}

/* The loop keeps v0 live across iterations: v0 += v1 (halfwords), then narrow it into the high half of v2 each time. */
static void loop_test(void) {
    uint8_t a[16], b[16], dst0[16], out[16], want[16];
    for (int i = 0; i < 16; i++) { a[i] = (uint8_t)next(); b[i] = (uint8_t)next(); dst0[i] = (uint8_t)(0xA0 + i); }
    uint64_t fpsr;
    __asm__ volatile(
        "msr fpsr, xzr\n ldr q0, [%[a]]\n ldr q1, [%[b]]\n ldr q2, [%[d]]\n mov x9, #64\n"
        "1: sqrshrun2 v2.16b, v0.8h, #4\n add v0.8h, v0.8h, v1.8h\n subs x9, x9, #1\n b.ne 1b\n"
        "str q2, [%[o]]\n mrs %[f], fpsr\n"
        : [f] "=r"(fpsr) : [a] "r"(a), [b] "r"(b), [d] "r"(dst0), [o] "r"(out) : "v0", "v1", "v2", "x9", "memory", "cc");
    uint16_t lane[8];
    int qc = 0;
    memcpy(lane, a, 16);
    for (int it = 0; it < 64; it++) {
        uint8_t src[16], narrowed[16];
        memcpy(src, lane, 16);
        int sat;
        model(5, 2, 4, 1, src, dst0, narrowed, &sat);
        qc |= sat;
        memcpy(want, narrowed, 16);
        for (int i = 0; i < 8; i++) lane[i] = (uint16_t)(lane[i] + ((uint16_t*)b)[i]);
    }
    checks++;
    if (memcmp(out, want, 16) || (int)((fpsr >> 27) & 1) != qc) { failures++; printf("FAIL loop with the source kept in a vector register\n"); }
}

int main(void) {
    struct Case cases[6 * (8 + 16 + 32) * 4];
    int ncases = 0;
    const int sizes[3] = {2, 4, 8};
    uint8_t* page = mmap(0, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) { printf("jit_shrn_sat: mmap failed\n"); return 1; }
    uint32_t* p = (uint32_t*)page;
    for (int subop = 0; subop < 6; subop++)
        for (int s = 0; s < 3; s++) {
            const int esize = sizes[s], dst_bytes = esize / 2;
            for (int shift = 1; shift <= dst_bytes * 8; shift++)
                for (int q = 0; q < 2; q++)
                    for (int alias = 0; alias < 2; alias++) {
                        /* immh:immb = source bits - shift; Rn = v0, Rd = v1 (or v0). */
                        const uint32_t imm7 = (uint32_t)(esize * 8 - shift);
                        const uint32_t insn = BASE[subop] | ((uint32_t)q << 30) | (imm7 << 16) | (0u << 5) | (alias ? 0u : 1u);
                        struct Case c = {subop, esize, shift, q, alias, p};
                        p = emit_stub(p, insn, alias);
                        cases[ncases++] = c;
                    }
        }
    if (mprotect(page, 1 << 20, PROT_READ | PROT_EXEC) != 0) { printf("jit_shrn_sat: mprotect failed\n"); return 1; }
    __builtin___clear_cache((char*)page, (char*)p);
    for (int i = 0; i < ncases; i++) {
        uint64_t list[64];
        const int n = lanes_for(cases[i].esize, cases[i].shift, list, 64);
        const int per = 16 / cases[i].esize;
        for (int batch = 0; batch * per < n; batch++) {
            uint8_t src[16] = {0}, dst0[16];
            for (int l = 0; l < per; l++) {
                const uint64_t v = list[(batch * per + l) % n];
                memcpy(src + l * cases[i].esize, &v, cases[i].esize);
            }
            for (int k = 0; k < 16; k++) dst0[k] = (uint8_t)(0x5A + 7 * k);
            check(&cases[i], src, dst0, batch);
        }
    }
    loop_test();
    if (failures) { printf("jit_shrn_sat: %d failures in %d checks\n", failures, checks); return 1; }
    printf("jit_shrn_sat: ALL PASS (%d checks)\n", checks);
    return 0;
}
