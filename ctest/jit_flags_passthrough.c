#include <stdint.h>
#include <stdio.h>
extern uint64_t taken_flags(uint64_t);
extern uint64_t fallthrough_flags(uint64_t);
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
__asm__(
".text\n.align 4\n.global taken_flags\n.type taken_flags,%function\n"
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
    puts("jit_flags_passthrough: 69152 checks passed");
}
