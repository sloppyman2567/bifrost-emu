#include <stdint.h>
#include <stdio.h>
extern uint64_t branch_loop(uint64_t);
extern uint64_t taken_flags(uint64_t);
extern uint64_t fallthrough_flags(uint64_t);
extern void arithmetic_flags(uint64_t, uint64_t, uint64_t *);
#define CHECK_BRANCH(name, instruction) \
    extern uint64_t name(uint64_t, uint64_t, uint64_t *); \
    __asm__(".text\n.align 4\n.global " #name "\n.type " #name ",%function\n" \
        #name ":\n msr nzcv,x1\n " instruction " 1f\n mov x3,#0\n b 2f\n" \
        "1: mov x3,#1\n2: stp x0,x3,[x2]\n mrs x0,nzcv\n ret\n" \
        ".size " #name ",.-" #name "\n");
CHECK_BRANCH(cbz_w, "cbz w0,")
CHECK_BRANCH(cbnz_w, "cbnz w0,")
CHECK_BRANCH(cbz_x, "cbz x0,")
CHECK_BRANCH(cbnz_x, "cbnz x0,")
CHECK_BRANCH(tbz_0, "tbz x0,#0,")
CHECK_BRANCH(tbnz_31, "tbnz x0,#31,")
CHECK_BRANCH(tbz_32, "tbz x0,#32,")
CHECK_BRANCH(tbnz_63, "tbnz x0,#63,")
// The conditional exit must commit all NZCV bits on both edges, including
// ADD's carry polarity and signed overflow, while preserving the result.
#define CHECK_EXIT(name, instruction) \
    extern void name(uint64_t, uint64_t, uint64_t *); \
    __asm__(".text\n.align 4\n.global " #name "\n.type " #name ",%function\n" \
        #name ":\n " instruction "\n b.eq 1f\n mov x5,#1\n b 2f\n" \
        "1: mov x5,#0\n2: mrs x5,nzcv\n stp x4,x5,[x2]\n ret\n" \
        ".size " #name ",.-" #name "\n");
CHECK_EXIT(add_exit_x, "adds x4,x0,x1")
CHECK_EXIT(sub_exit_x, "subs x4,x0,x1")
CHECK_EXIT(add_exit_w, "adds w4,w0,w1")
CHECK_EXIT(sub_exit_w, "subs w4,w0,w1")
__asm__(
".text\n.align 4\n.global arithmetic_flags\n.type arithmetic_flags,%function\n"
"arithmetic_flags:\n cmp x0,x1\n add x4,x0,x1\n add x0,x0,x1\n"
"add x1,x0,x1\n add x4,x4,x4\n add x5,x4,#4095\n sub x5,x5,#4095\n"
"add w6,w0,w1\n cset x7,lt\n cset x8,cs\n mrs x9,nzcv\n"
"stp x0,x1,[x2]\n stp x4,x5,[x2,#16]\n stp x6,x7,[x2,#32]\n"
"stp x8,x9,[x2,#48]\n ret\n.size arithmetic_flags,.-arithmetic_flags\n"
".text\n.align 4\n.global branch_loop\n.type branch_loop,%function\n"
"branch_loop:\n mov x1,#0\n1: cbz x0,3f\n add x1,x1,x0\n sub x0,x0,#1\n b 1b\n"
"3: mov x0,x1\n ret\n.size branch_loop,.-branch_loop\n"
".align 4\n.global taken_flags\n.type taken_flags,%function\n"
"taken_flags:\n mov x1,#0\n cmp x0,#0\n b.eq 1f\n mov x2,#0\n b 1f\n"
"1: cbz x1,2f\n b 2f\n"
"2: cset w0,eq\n ret\n"
".size taken_flags,.-taken_flags\n"
".align 4\n.global fallthrough_flags\n.type fallthrough_flags,%function\n"
"fallthrough_flags:\n mov x1,#0\n cmp x0,#0\n b.ne 3f\n"
"1: cbz x1,2f\n b 2f\n"
"2: cset w0,eq\n ret\n"
"3: mov x2,#0\n b 1b\n"
".size fallthrough_flags,.-fallthrough_flags\n");
int main(void) {
    uint64_t (*branches[])(uint64_t, uint64_t, uint64_t *) = {
        cbz_w, cbnz_w, cbz_x, cbnz_x, tbz_0, tbnz_31, tbz_32, tbnz_63
    };
    const uint64_t values[] = {0, 1, UINT64_C(0x80000000), UINT64_C(0x100000000),
                              UINT64_C(0x8000000000000000), UINT64_MAX};
    void (*exits[])(uint64_t, uint64_t, uint64_t *) = {
        add_exit_x, sub_exit_x, add_exit_w, sub_exit_w
    };
    for (unsigned repeat = 0; repeat < 64; ++repeat) {
        for (unsigned op = 0; op < 4; ++op)
            for (unsigned i = 0; i < 6; ++i) for (unsigned j = 0; j < 6; ++j) {
                unsigned bits = op < 2 ? 64 : 32, sub = op & 1;
                uint64_t a = values[i], b = values[j];
                if (bits == 32) { a = (uint32_t)a; b = (uint32_t)b; }
                uint64_t result = sub ? a - b : a + b;
                if (bits == 32) result = (uint32_t)result;
                unsigned n = result >> (bits - 1), z = result == 0;
                unsigned c = sub ? a >= b : bits == 32 ? a + b > UINT32_MAX : result < a;
                unsigned v = ((sub ? a ^ b : ~(a ^ b)) & (a ^ result)) >> (bits - 1) & 1;
                uint64_t flags = (uint64_t)(n << 3 | z << 2 | c << 1 | v) << 28;
                uint64_t got[2];
                exits[op](values[i], values[j], got);
                if (got[0] != result || got[1] != flags) {
                    printf("exit flags op=%u i=%u j=%u result=%llx flags=%llx want=%llx/%llx\n",
                        op, i, j, (unsigned long long)got[0], (unsigned long long)got[1],
                        (unsigned long long)result, (unsigned long long)flags);
                    return 1;
                }
            }
    }
    for (unsigned repeat = 0; repeat < 64; ++repeat) {
        for (unsigned i = 0; i < 6; ++i) for (unsigned j = 0; j < 6; ++j) {
            uint64_t a = values[i], b = values[j], diff = a - b;
            unsigned n = diff >> 63, z = diff == 0, c = a >= b;
            unsigned v = ((a ^ b) & (a ^ diff)) >> 63;
            const uint64_t want[] = {a + b, a + b + b, 2 * (a + b),
                2 * (a + b), (uint32_t)(2 * a + 3 * b), n ^ v, c,
                (uint64_t)(n << 3 | z << 2 | c << 1 | v) << 28};
            uint64_t got[8];
            arithmetic_flags(a, b, got);
            for (unsigned k = 0; k < 8; ++k) if (got[k] != want[k]) {
                printf("arithmetic flags a=%llx b=%llx result=%u got=%llx want=%llx\n",
                    (unsigned long long)a, (unsigned long long)b, k,
                    (unsigned long long)got[k], (unsigned long long)want[k]);
                return 1;
            }
        }
    }
    for (unsigned repeat = 0; repeat < 64; ++repeat) {
        for (unsigned f = 0; f < 16; ++f) for (unsigned v = 0; v < 6; ++v) {
            uint64_t value = values[v], flags = (uint64_t) f << 28;
            const unsigned taken[] = {!(uint32_t)value, !!(uint32_t)value,
                !value, !!value, !(value & 1), (unsigned)((value >> 31) & 1),
                !(value & (UINT64_C(1) << 32)), (unsigned)(value >> 63)};
            for (unsigned b = 0; b < 8; ++b) {
                uint64_t observed[2] = {0, 0};
                uint64_t result = branches[b](value, flags, observed);
                if (result != flags || observed[0] != value || observed[1] != taken[b]) {
                    printf("branch passthrough b=%u flags=%x value=%llx result=%llx seen=%llx taken=%llu\n",
                        b, f, (unsigned long long)value, (unsigned long long)result,
                        (unsigned long long)observed[0], (unsigned long long)observed[1]);
                    return 1;
                }
            }
        }
    }
    for (unsigned i = 0; i < 10000; ++i) {
        unsigned input = i & 1;
        uint64_t a = taken_flags(input), b = fallthrough_flags(input);
        if (a != !input || b != !input) {
            printf("flag passthrough i=%u taken=%llu fallthrough=%llu want=%u\n",
                   i, (unsigned long long)a, (unsigned long long)b, !input);
            return 1;
        }
    }
    for (unsigned repeat = 0; repeat < 64; ++repeat) {
        for (uint64_t n = 0; n < 258; ++n) {
            uint64_t got = branch_loop(n), want = n * (n + 1) / 2;
            if (got != want) {
                printf("branch loop n=%llu got=%llu want=%llu\n",
                    (unsigned long long)n, (unsigned long long)got, (unsigned long long)want);
                return 1;
            }
        }
    }
    puts("jit_flags_passthrough: 113312 checks passed");
}
