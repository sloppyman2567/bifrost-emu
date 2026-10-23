/*
 * jit_simd_minmax.c - integer vector UMAX/UMIN/SMAX/SMIN against a lane model.
 *
 * Every element size and both vector widths run with Rd distinct from, equal
 * to Rn and equal to Rm. The lane model follows the Arm ARM pseudocode (a
 * comparison of the zero- or sign-extended lanes); results must match, and
 * a Q=0 result must clear the upper half. Unsigned halfword lanes (.4h/.8h)
 * used to leave the JIT and run through the interpreter.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

typedef struct { uint8_t b[16]; } V;
typedef void (*stub_fn)(const V*, const V*, const V*, V*);

static uint32_t* stub;

/* ldr q0,[x0]; ldr q1,[x1]; ldr q2,[x2]; <insn>; str q<rd>,[x3]; ret */
static stub_fn install(uint32_t insn, unsigned rd) {
    stub[0] = 0x3dc00000; stub[1] = 0x3dc00021; stub[2] = 0x3dc00042;
    stub[3] = insn;
    stub[4] = 0x3d800060 | rd;
    stub[5] = 0xd65f03c0;
    __builtin___clear_cache((char*)stub, (char*)(stub + 6));
    return (stub_fn)stub;
}

static uint64_t seed = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) {
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    return seed;
}

static uint64_t lane_get(const V* v, int i, int bytes) {
    uint64_t x = 0; memcpy(&x, v->b + i * bytes, bytes); return x;
}
static void lane_put(V* v, int i, int bytes, uint64_t x) { memcpy(v->b + i * bytes, &x, bytes); }
static int64_t sext(uint64_t x, int bytes) {
    const int s = 64 - 8 * bytes; return (int64_t)(x << s) >> s;
}

/* kind: 0 UMAX, 1 UMIN, 2 SMAX, 3 SMIN */
static void model(int kind, int bytes, int q, const V* n, const V* m, V* out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < (q ? 16 : 8) / bytes; ++i) {
        const uint64_t a = lane_get(n, i, bytes), b = lane_get(m, i, bytes);
        uint64_t r;
        if (kind < 2) r = (kind == 0) == (a > b) ? a : b;
        else r = (kind == 2) == (sext(a, bytes) > sext(b, bytes)) ? a : b;
        lane_put(out, i, bytes, r);
    }
}

static void edge(V* v, int bytes, unsigned k) {
    static const uint64_t pool[] = {0, 1, 2, 0x7e, 0x7f, 0x80, 0x81, 0xfe, 0xff};
    for (int i = 0; i < 16 / bytes; ++i) {
        const uint64_t top = 1ull << (8 * bytes - 1), mask = bytes == 8 ? ~0ull : (1ull << (8 * bytes)) - 1;
        uint64_t x;
        switch ((k + i) % 6) {
            case 0: x = pool[(k / 6 + i) % 9]; break;
            case 1: x = top; break;
            case 2: x = top - 1; break;
            case 3: x = mask; break;
            case 4: x = mask - (k % 3); break;
            default: x = top + ((k / 6 + i) % 5) - 2; break;
        }
        lane_put(v, i, bytes, x & mask);
    }
}

static void dump(const char* label, const V* v) {
    for (int i = 0; i < 16; ++i) printf("%02x", v->b[i]);
    printf(" %s\n", label);
}

int main(void) {
    stub = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    static const uint32_t base[4] = {0x2e206400, 0x2e206c00, 0x0e206400, 0x0e206c00};
    static const char* const name[4] = {"umax", "umin", "smax", "smin"};
    unsigned checks = 0, fails = 0;
    for (int kind = 0; kind < 4; ++kind)
    for (int size = 0; size < 3; ++size)
    for (int q = 0; q < 2; ++q)
    for (int alias = 0; alias < 3; ++alias) {
        const int bytes = 1 << size;
        const unsigned rd = alias == 0 ? 2 : alias == 1 ? 0 : 1;
        const uint32_t insn = base[kind] | (uint32_t)size << 22 | (uint32_t)q << 30 | 1u << 16 | 0u << 5 | rd;
        stub_fn fn = install(insn, rd);
        for (unsigned k = 0; k < 6000; ++k) {
            V n, m, d, got, want;
            if (k < 3000) {
                edge(&n, bytes, k);
                edge(&m, bytes, k / 7 + 5 * (k % 11));
            } else {
                for (int i = 0; i < 16; ++i) { n.b[i] = (uint8_t)rnd(); m.b[i] = (uint8_t)rnd(); }
            }
            for (int i = 0; i < 16; ++i) d.b[i] = (uint8_t)(0xa5 + i);
            fn(&n, &m, &d, &got);
            model(kind, bytes, q, &n, &m, &want);
            ++checks;
            if (memcmp(&got, &want, sizeof got)) {
                if (++fails <= 8) {
                    printf("FAIL %s size=%d q=%d alias=%d k=%u\n", name[kind], size, q, alias, k);
                    dump("n", &n); dump("m", &m); dump("got", &got); dump("want", &want);
                }
            }
        }
    }
    if (fails) { printf("jit_simd_minmax: %u of %u checks FAILED\n", fails, checks); return 1; }
    printf("jit_simd_minmax: %u checks passed\n", checks);
    return 0;
}
