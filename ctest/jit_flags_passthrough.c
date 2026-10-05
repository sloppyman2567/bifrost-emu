#include <stdint.h>
#include <stdio.h>
extern uint64_t taken_flags(uint64_t);
extern uint64_t fallthrough_flags(uint64_t);
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
    for (unsigned i = 0; i < 10000; ++i) {
        unsigned input = i & 1;
        uint64_t a = taken_flags(input), b = fallthrough_flags(input);
        if (a != !input || b != !input) {
            printf("flag passthrough i=%u taken=%llu fallthrough=%llu want=%u\n",
                   i, (unsigned long long)a, (unsigned long long)b, !input);
            return 1;
        }
    }
    puts("jit_flags_passthrough: 20000 checks passed");
}
