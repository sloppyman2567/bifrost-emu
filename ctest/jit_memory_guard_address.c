// Permission guards must not retain address mappings in their scratch regs.
#include <stdint.h>
#include <stdio.h>
extern uint64_t guard_values[8];
__asm__(".data\n.balign 4096\n.zero 200\n.global guard_values\nguard_values:\n"
        ".quad 11,22,33,44,55,0x123456789abcdef0,77,88\n"
        ".balign 4096\n.zero 2968\n.global guard_pointer\nguard_pointer:\n.quad guard_values\n");
extern uint64_t probe(void);
__asm__(
    ".text\n.global probe\n.type probe,%function\nprobe:\n"
    "stp x29,x30,[sp,#-144]!\nmov x29,sp\n"
    "stp x21,x22,[sp,#32]\nadrp x21,guard_pointer\n"
    "stp x19,x20,[sp,#16]\nadrp x19,guard_values\n"
    "add x0,x19,:lo12:guard_values\n"
    "ldr x3,[x21,:lo12:guard_pointer]\n"
    "ldr x2,[x0,#40]\nldr x4,[x3]\nstr x4,[sp,#136]\n"
    "mov x4,#0\ncbz x2,1f\nldr x4,[sp,#136]\neor x0,x2,x4\n"
    "b 2f\n1: mov x0,#0\n2:\n"
    "ldp x19,x20,[sp,#16]\nldp x21,x22,[sp,#32]\n"
    "ldp x29,x30,[sp],#144\nret\n.size probe,.-probe\n");
int main(void) {
    for(int i=0;i<10000;++i) {
        uint64_t got=probe(), want=guard_values[0]^guard_values[5];
        if(got!=want) { printf("FAIL: guard address got=%llx want=%llx\n",
            (unsigned long long)got,(unsigned long long)want); return 1; }
    }
    puts("ALL PASS: memory guard cached address preservation");
    return 0;
}
