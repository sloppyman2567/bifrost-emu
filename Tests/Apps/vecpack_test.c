/* vecpack_test.c — reproduce xcb_create_window's SIMD packing.
 * Packing sequence from libxcb.so xcb_create_window (offset 0x12230):
 *   and w6,w6,#0xffff   ; width low 16
 *   fmov d0,x6          ; width -> v0.h[0]
 *   mov v0.h[1], w7     ; height -> v0.h[1]  (mov from gpr lane of vector; here w7)
 *   ld1 {v0.h}[2], [x0] ; border load h0 from [x0] -> v0.h[2]
 *   mov v0.h[3], v1.h[0]; class from v1.h[0]   -> v0.h[3]
 *   str d0, [x1]        ; store
 * We mimic with border src byte and class in an fp reg.
 * Expected v0.8h (LE): h0=0x0208(520), h1=0x0140(320), h2=0x0007, h3=0x0101
 * out bytes = 08 02 40 01 07 00 01 01
 */
#include <stdio.h>
#include <stdint.h>

static void pack_asm(uint64_t* out) {
    uint8_t border_src = 7;    /* loaded into s register then v0.h[2] via ld1 */
    register uint64_t w6 __asm__("w6") = 520;
    register uint64_t w7 __asm__("w7") = 320;
    __asm__ volatile(
        "and w6, w6, #0xffff\n\t"
        "fmov d0, x6\n\t"
        "mov v0.h[1], w7\n\t"
        "fmov s1, %w[cls]\n\t"
        "ld1 {v0.h}[2], [%[bsrc]]\n\t"
        "mov v0.h[3], v1.h[0]\n\t"
        "str d0, [%[out]]\n\t"
        : : [cls]"r"(1u), [bsrc]"r"(&border_src), [out]"r"(out)
        : "v0","v1","memory");
}

int main(void) {
    uint64_t out = 0;
    pack_asm(&out);
    printf("packed=%016llx\nbytes=", (unsigned long long)out);
    for(int i=0;i<8;i++) printf(" %02x", (unsigned)((out>>(8*i))&0xff));
    printf("\n");
    return 0;
}