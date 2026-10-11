// Permission guards must not retain address mappings in their scratch regs.
#include <stdint.h>
#include <stdio.h>
extern uint64_t guard_values[8];
__asm__(".data\n.balign 4096\n.zero 200\n.global guard_values\nguard_values:\n"
        ".quad 11,22,33,44,55,0x123456789abcdef0,77,88\n"
        ".balign 4096\n.zero 2968\n.global guard_pointer\nguard_pointer:\n.quad guard_values\n");
extern uint64_t probe(void);
extern uint64_t pressure(const uint64_t *, uint64_t *);
__asm__(".text\n.align 4\n.global pressure\n.type pressure,%function\npressure:\n"
    "ldr x3,[x0]\nldr x4,[x0,#8]\nldr x5,[x0,#16]\nldr x6,[x0,#24]\n"
    "ldr x7,[x0,#32]\nldr x8,[x0,#40]\nldr x9,[x0,#48]\nldr x10,[x0,#56]\n"
    "ldr x11,[x0,#64]\nldr x12,[x0,#72]\nadd x0,x0,#8\n"
    "ldurb w13,[x0,#-1]\nldurh w14,[x0,#-6]\nldr w15,[x0,#4]\nldr x16,[x0,#8]\n"
    "strb w3,[x1]\nstrh w4,[x1,#2]\nstr w5,[x1,#4]\nstr x6,[x1,#8]\n"
    "eor x3,x3,x13\nadd x4,x4,x14\neor x5,x5,x15\nadd x6,x6,x16\n"
    "add x3,x3,x4\neor x3,x3,x5\nadd x3,x3,x6\neor x3,x3,x7\n"
    "add x3,x3,x8\neor x3,x3,x9\nadd x3,x3,x10\neor x3,x3,x11\n"
    "add x0,x3,x12\nret\n.size pressure,.-pressure\n");
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
    for (unsigned r = 1; r <= 256; ++r) {
        uint64_t in[10], out[2] = {0, 0};
        for (unsigned i = 0; i < 10; ++i)
            in[i] = UINT64_C(0xfedcba9876543210) * (i + r);
        uint64_t want = in[0] ^ (in[0] >> 56);
        want += in[1] + ((in[0] >> 16) & 0xffff);
        want ^= in[2] ^ (in[1] >> 32);
        want += in[3] + in[2];
        want ^= in[4]; want += in[5]; want ^= in[6];
        want += in[7]; want ^= in[8]; want += in[9];
        uint64_t got = pressure(in, out);
        uint64_t packed = (in[0] & 0xff) | ((in[1] & 0xffff) << 16) |
                          ((in[2] & 0xffffffff) << 32);
        if (got != want || out[0] != packed || out[1] != in[3]) {
            printf("FAIL: guarded memory pressure repeat=%u\n", r);
            return 1;
        }
    }
    for(int i=0;i<10000;++i) {
        uint64_t got=probe(), want=guard_values[0]^guard_values[5];
        if(got!=want) { printf("FAIL: guard address got=%llx want=%llx\n",
            (unsigned long long)got,(unsigned long long)want); return 1; }
    }
    puts("ALL PASS: memory guard cached address preservation");
    return 0;
}
