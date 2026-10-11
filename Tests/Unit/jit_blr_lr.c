// BLR X30 must read its target before writing the link register. ART nterp
// uses this form for quick entry points; a skipped call leaves its ArtMethod
// pointer in X0 in place of the callee's return value.
#include <stdint.h>
#include <stdio.h>

extern uint64_t call_lr(uint64_t);
extern uint64_t call_scratch(uint64_t);
__asm__(
".text\n.align 4\n.global call_lr\n.type call_lr,%function\n"
"call_lr:\n stp x29,x30,[sp,#-16]!\n"
" adr x30,1f\n blr x30\n"
"2: adr x1,2b\n cmp x1,x30\n b.eq 3f\n mov x0,#0\n"
"3: ldp x29,x30,[sp],#16\n ret\n"
"1: add x0,x0,#7\n ret\n.size call_lr,.-call_lr\n"
".align 4\n.global call_scratch\n.type call_scratch,%function\n"
"call_scratch:\n stp x29,x30,[sp,#-16]!\n"
" adr x16,1f\n blr x16\n"
"2: adr x1,2b\n cmp x1,x30\n b.eq 3f\n mov x0,#0\n"
"3: ldp x29,x30,[sp],#16\n ret\n"
"1: add x0,x0,#7\n ret\n.size call_scratch,.-call_scratch\n");

int main(void) {
    for (uint64_t i = 0; i < 10000; ++i) {
        uint64_t lr = call_lr(i), scratch = call_scratch(i);
        if (lr != i + 7 || scratch != i + 7) {
            printf("BLR i=%llu lr=%llu scratch=%llu expected=%llu\n",
                   (unsigned long long)i, (unsigned long long)lr,
                   (unsigned long long)scratch, (unsigned long long)(i + 7));
            return 1;
        }
    }
    puts("jit_blr_lr: 20000 checks passed");
    return 0;
}
