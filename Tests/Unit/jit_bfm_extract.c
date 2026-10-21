/* Exhaustive UBFM/SBFM (ubfx, sbfx, ubfiz, sbfiz, lsr, asr, lsl, uxt*, sxt*):
 * every immr/imms pair, both widths, several inputs, with the destination both
 * aliasing and distinct from the source. Expected values follow the Arm ARM
 * pseudocode (DecodeBitMasks, bitfield move); run under QEMU to check it. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

static uint64_t ones(unsigned n) { return n >= 64 ? ~0ULL : (1ULL << n) - 1; }
static uint64_t ror(uint64_t x, unsigned r, unsigned size) {
    uint64_t m = ones(size);
    x &= m;
    return r ? ((x >> r) | (x << (size - r))) & m : x;
}
/* Bitfield move per the Arm ARM: UBFM/SBFM, datasize 32 or 64. */
static uint64_t bfm(int sign, unsigned size, unsigned r, unsigned s, uint64_t src) {
    uint64_t m = ones(size);
    src &= m;
    uint64_t wmask = ror(ones(s + 1), r, size);
    unsigned d = (s - r) & (size - 1);
    uint64_t tmask = ones(d + 1);
    uint64_t bot = ror(src, r, size) & wmask;
    uint64_t top = (sign && ((src >> s) & 1)) ? m : 0;
    return (((top & ~tmask) | (bot & tmask)) & m);
}

int main(void) {
    const uint64_t values[] = {0, ~0ULL, 0x0123456789abcdefULL, 0xfedcba9876543210ULL,
                               0x8000000000000001ULL, 0x00000000ffffffffULL, 0xffffffff00000000ULL,
                               0x80000000ULL, 0x7fffffffULL, 0xaaaaaaaaaaaaaaaaULL,
                               0x5555555555555555ULL, 0x00000000deadbeefULL};
    const unsigned nvalues = sizeof(values) / sizeof(values[0]);
    uint32_t* code = mmap(0, 1 << 23, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 1;
    /* [sign][width][alias][r][s] -> one 2-word function each. */
    unsigned slot = 0;
    for (int sign = 0; sign < 2; ++sign)
        for (unsigned width = 32; width <= 64; width += 32)
            for (int alias = 0; alias < 2; ++alias)
                for (unsigned r = 0; r < width; ++r)
                    for (unsigned s = 0; s < width; ++s) {
                        uint32_t base = sign ? (width == 64 ? 0x93400000u : 0x13000000u)
                                             : (width == 64 ? 0xd3400000u : 0x53000000u);
                        code[slot++] = base | (r << 16) | (s << 10) | (alias ? 0u : (1u << 5));
                        code[slot++] = 0xd65f03c0u;
                    }
    __builtin___clear_cache((char*)code, (char*)(code + slot));
    slot = 0;
    unsigned checks = 0;
    for (int sign = 0; sign < 2; ++sign)
        for (unsigned width = 32; width <= 64; width += 32)
            for (int alias = 0; alias < 2; ++alias)
                for (unsigned r = 0; r < width; ++r)
                    for (unsigned s = 0; s < width; ++s) {
                        uint64_t (*fn)(uint64_t, uint64_t) = (void*)(code + slot);
                        slot += 2;
                        for (unsigned i = 0; i < nvalues; ++i)
                            for (unsigned j = 0; j < nvalues; ++j) {
                                /* alias: source and destination are x0. */
                                uint64_t src = alias ? values[i] : values[j];
                                uint64_t want = bfm(sign, width, r, s, src);
                                uint64_t got = fn(values[i], values[j]);
                                ++checks;
                                if (got != want) {
                                    printf("%cBFM %u alias=%d r=%u s=%u src=%llx got=%llx want=%llx\n",
                                           sign ? 'S' : 'U', width, alias, r, s,
                                           (unsigned long long)src, (unsigned long long)got,
                                           (unsigned long long)want);
                                    return 1;
                                }
                            }
                    }
    /* Register pressure: keep L temporaries live across the bitfield op so its
     * result lands in the allocator's later registers (r8..r15), whose
     * encodings differ from rax/rcx/rdx. Result = field + sum of temporaries. */
    const unsigned live_counts[] = {0, 3, 6, 8, 9};
    const unsigned pv = 3; /* inputs per case */
    unsigned pslot = 0;
    for (int sign = 0; sign < 2; ++sign)
        for (unsigned width = 32; width <= 64; width += 32)
            for (unsigned li = 0; li < sizeof(live_counts) / sizeof(live_counts[0]); ++li)
                for (unsigned r = 0; r < width; ++r)
                    for (unsigned s = 0; s < width; ++s) {
                        uint32_t base = sign ? (width == 64 ? 0x93400000u : 0x13000000u)
                                             : (width == 64 ? 0xd3400000u : 0x53000000u);
                        const unsigned L = live_counts[li];
                        /* x9..x(8+L) = x2 + k (k = 1..L): independent live values. */
                        for (unsigned k = 1; k <= L; ++k) code[pslot++] = 0x91000000u | (k << 10) | (2u << 5) | (8 + k);
                        code[pslot++] = base | (r << 16) | (s << 10) | (1u << 5); /* x0 = bfm(x1) */
                        for (unsigned k = 1; k <= L; ++k) code[pslot++] = 0x8b000000u | ((8 + k) << 16) | (0u << 5) | 0u; /* x0 += x(8+k) */
                        code[pslot++] = 0xd65f03c0u;
                    }
    __builtin___clear_cache((char*)code, (char*)(code + pslot));
    pslot = 0;
    for (int sign = 0; sign < 2; ++sign)
        for (unsigned width = 32; width <= 64; width += 32)
            for (unsigned li = 0; li < sizeof(live_counts) / sizeof(live_counts[0]); ++li)
                for (unsigned r = 0; r < width; ++r)
                    for (unsigned s = 0; s < width; ++s) {
                        const unsigned L = live_counts[li];
                        uint64_t (*fn)(uint64_t, uint64_t, uint64_t) = (void*)(code + pslot);
                        pslot += 2 * L + 2;
                        for (unsigned i = 0; i < pv; ++i) {
                            uint64_t src = values[i * 3], add = values[i + 1];
                            uint64_t want = bfm(sign, width, r, s, src);
                            for (unsigned k = 1; k <= L; ++k) want += add + k;
                            uint64_t got = fn(0, src, add);
                            ++checks;
                            if (got != want) {
                                printf("pressure %cBFM %u live=%u r=%u s=%u got=%llx want=%llx\n",
                                       sign ? 'S' : 'U', width, L, r, s,
                                       (unsigned long long)got, (unsigned long long)want);
                                return 1;
                            }
                        }
                    }
    munmap(code, 1 << 23);
    printf("jit_bfm_extract: %u checks passed\n", checks);
    return 0;
}
