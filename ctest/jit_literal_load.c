// ART emits PC-relative literal loads, unlike most compiler-generated ELF
// code. Cover both displacement signs, GPR extension and vector zeroing.
#include <stdint.h>
#include <stdio.h>
static int checks, failures;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    printf("FAIL line %d: %s\n", __LINE__, #expr); ++failures; } } while (0)
#define FORWARD(load, data) load " 1f\n b 2f\n .balign 16\n1: " data "\n2:\n"
#define BACKWARD(load, data) "b 2f\n .balign 16\n1: " data "\n2: " load " 1b\n"
#define TEST_GPR(sequence) do { \
    uint64_t w, x, sw; \
    __asm__ volatile(sequence("ldr %w0,", ".word 0xfedcba98") : "=r"(w)); \
    __asm__ volatile(sequence("ldr %0,", ".quad 0xfedcba9876543210") : "=r"(x)); \
    __asm__ volatile(sequence("ldrsw %0,", ".word 0xfedcba98") : "=r"(sw)); \
    CHECK(w == UINT64_C(0xfedcba98)); \
    CHECK(x == UINT64_C(0xfedcba9876543210)); \
    CHECK(sw == UINT64_C(0xfffffffffedcba98)); \
} while (0)
#define TEST_VECTOR(sequence) do { \
    uint64_t out[2]; \
    __asm__ volatile("movi v0.16b,#255\n" \
        sequence("ldr s0,", ".word 0xfedcba98") "str q0,[%0]" \
        : : "r"(out) : "v0", "memory"); \
    CHECK(out[0] == UINT64_C(0xfedcba98) && out[1] == 0); \
    __asm__ volatile("movi v0.16b,#255\n" \
        sequence("ldr d0,", ".quad 0xfedcba9876543210") "str q0,[%0]" \
        : : "r"(out) : "v0", "memory"); \
    CHECK(out[0] == UINT64_C(0xfedcba9876543210) && out[1] == 0); \
    __asm__ volatile(sequence("ldr q0,", ".quad 0x123456789abcdef0,0xfedcba9876543210") \
        "str q0,[%0]" : : "r"(out) : "v0", "memory"); \
    CHECK(out[0] == UINT64_C(0x123456789abcdef0)); \
    CHECK(out[1] == UINT64_C(0xfedcba9876543210)); \
} while (0)
extern uint64_t literal_cache0(const uint64_t *);
extern uint64_t literal_cache1(const uint64_t *);
extern uint64_t literal_cache2(const uint64_t *);
// Three functions at the same index of the 8192-set, two-way decode cache:
// evict a literal decode with an ordinary load and check its base resets.
__asm__(
".text\n.p2align 15\n.global literal_cache0\nliteral_cache0:\n"
"ldr x0,1f\nret\n1: .quad 123\n"
".p2align 15\n.global literal_cache1\nliteral_cache1:\n"
"ldr x0,1f\nret\n1: .quad 456\n"
".p2align 15\n.global literal_cache2\nliteral_cache2:\n"
"ldr x0,[x0]\nret\n");
int main(void) {
    const uint64_t value = UINT64_C(0x123456789abcdef0);
    for (unsigned i = 0; i < 32; ++i) {
        CHECK(literal_cache0(&value) == 123);
        CHECK(literal_cache1(&value) == 456);
        CHECK(literal_cache2(&value) == value);
    }
    TEST_GPR(FORWARD);
    TEST_GPR(BACKWARD);
    TEST_VECTOR(FORWARD);
    TEST_VECTOR(BACKWARD);
    // Rt=31 still loads memory (discards its result); PRFM does not.
    __asm__ volatile(FORWARD("ldr xzr,", ".quad 1"));
    __asm__ volatile(".inst 0xd87fffe0"); // PRFM literal +1048572 (unmapped)
    CHECK(1);
    printf("jit_literal_load: %d checks passed, %d failures\n", checks - failures, failures);
    return failures != 0;
}
