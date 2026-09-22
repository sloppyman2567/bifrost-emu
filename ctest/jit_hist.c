// Histogram/count-table pattern (zlib inflate_table hunt):
//   ldrh w1,[x0],#2 ; lsl x1,x1,#1 ; ldrh w3,[x14,x1] ;
//   add w3,#1 ; strh w3,[x14,x1]
// Compare full table JIT vs --no-jit; any mismatch = real miscompile
// (register-offset store, invisible to JIT_VERIFY's snapshot).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint16_t cnt[288];
static uint8_t lens[1024];

int main(void) {
    for (int i = 0; i < 1024; i++) lens[i] = (uint8_t)((i * 37 + 11) % 19);
    memset(cnt, 0, sizeof(cnt));
    uint16_t *c = cnt;
    uint8_t *p = lens;
    uint8_t *end = lens + 1024;
    // Force the exact shape: post-index ldrh, lsl, indexed strh.
    __asm__ volatile (
        "0: ldrh w1, [%1], #2\n"
        "   lsl x1, x1, #1\n"
        "   ldrh w3, [%0, x1]\n"
        "   add w3, w3, #1\n"
        "   strh w3, [%0, x1]\n"
        "   cmp %1, %2\n"
        "   b.ne 0b\n"
        : "+r"(c), "+r"(p) : "r"(end) : "w1", "w3", "memory", "cc");
    uint32_t h = 0;
    for (int i = 0; i < 288; i++) { printf("%u%c", cnt[i], i == 287 ? '\n' : ' '); h = h * 31 + cnt[i]; }
    printf("hash=0x%08x\n", h);
    return 0;
}
